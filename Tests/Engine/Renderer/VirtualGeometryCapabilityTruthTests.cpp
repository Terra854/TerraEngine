#include "TestHarness.h"

#include "Renderer/RHI/RHIDefinitions.h"
#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"

namespace TerraEngine::Tests
{

	void RegisterVirtualGeometryCapabilityTruthTests(TestRegistry &registry)
	{
		registry.Add("Renderer.VirtualGeometryCapabilityTruth.RejectsPreparedOnlyCompaction",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryCapabilityEvidence evidence{};
				evidence.planned = true;
				evidence.supported = true;
				evidence.executed = true;
				evidence.execution.completedLiveCommand = true;
				evidence.execution.preparedOnly = true;
				evidence.execution.evidenceId = "cpu-prepared-compaction";

				context.Expect(!evidence.CanReportExecuted(), "CPU-prepared compaction must not be reported as executed Vulkan work.");
			});

		registry.Add("Renderer.VirtualGeometryCapabilityTruth.RejectsPreparedOnlyGeneratedSubmission",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt receipt{};
				receipt.stage = TerraEngine::Renderer::VirtualGeometryBackendStage::IndirectGeneration;
				receipt.route = TerraEngine::Renderer::VirtualGeometryBackendRoute::MeshShader;
				receipt.frameNumber = 71u;
				receipt.rendererGeneration = 4u;
				receipt.geometryGeneration = 9u;
				receipt.commandGeneration = 12u;
				receipt.completionFence = 3u;
				receipt.commandRecorded = true;
				receipt.commandSubmitted = false;
				receipt.completionObserved = false;
				receipt.generationMatched = true;
				receipt.evidenceId = "cpu-prepared-generated-submission";

				context.Expect(!receipt.IsExecuted(),
					"A CPU-expanded generated-submission mirror must not become an executed receipt "
					"before a live Vulkan submit and completion.");
			});
	}

} // namespace TerraEngine::Tests
