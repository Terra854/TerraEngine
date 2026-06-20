#pragma once

#include "Renderer/Graph/RenderSubmissionPlan.h"

namespace TerraEngine::Tests {

	struct MockRenderBackend
	{
		TerraEngine::Renderer::BackendCapabilityProfile profile;
		TerraEngine::Renderer::RenderSubmissionPlan lastPlan;

		MockRenderBackend()
		{
			profile.backendId = "mock";
			profile.supportsParallelRecording = true;
			profile.supportsAsyncUploadPreparation = true;
			profile.supportsTransientResources = true;
			profile.supportsRenderTaskGroups = true;
			profile.supportsPacketLocalDiagnostics = true;
			profile.maxRecordingWorkers = 4u;
			profile.workerSchedulingThreshold = 8u;
			profile.requiredSerialBoundaries = { "final-submit", "present" };
			profile.diagnosticCapabilities = { "packet-counts", "serialized-reasons" };
		}

		bool Consume(const TerraEngine::Renderer::RenderSubmissionPlan& plan)
		{
			lastPlan = plan;
			return plan.valid;
		}
	};

}
