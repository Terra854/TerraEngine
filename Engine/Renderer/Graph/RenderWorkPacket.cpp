#include "RenderWorkPacket.h"

namespace TerraEngine::Renderer {

	const char* ToString(RenderBackendWorkKind workKind)
	{
		switch (workKind)
		{
		case RenderBackendWorkKind::Geometry: return "geometry";
		case RenderBackendWorkKind::Shadow: return "shadow";
		case RenderBackendWorkKind::UI: return "ui";
		case RenderBackendWorkKind::Culling: return "culling";
		case RenderBackendWorkKind::Descriptor: return "descriptor";
		case RenderBackendWorkKind::Upload: return "upload";
		case RenderBackendWorkKind::Transition: return "transition";
		case RenderBackendWorkKind::Validation: return "validation";
		case RenderBackendWorkKind::Stats: return "stats";
		default: return "unknown";
		}
	}

	BackendRecordingBatchPlan BuildBackendRecordingBatchPlan(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const BackendCapabilityProfile& capabilities,
		RenderBackendWorkKind workKind)
	{
		BackendRecordingBatchPlan plan;
		for (const RenderWorkPacket& packet : packetPlan.packets)
		{
			BackendRecordingBatch batch;
			batch.batchId = "frame-" + std::to_string(snapshot.frameId) + "." + packet.packetId + "." + ToString(workKind);
			batch.frameId = snapshot.frameId;
			batch.passId = packet.passId;
			batch.workKind = workKind;
			batch.begin = packet.begin;
			batch.end = packet.end;
			batch.workerEligible = packet.executionPolicy == RenderPacketExecutionPolicy::Worker && capabilities.supportsParallelRecording;
			if (batch.workerEligible)
			{
				++plan.workerBatchCount;
			}
			else
			{
				++plan.serialBatchCount;
				batch.fallbackReason = "Backend recording batch requires coordinator execution or a serial fallback.";
				plan.serializedReasons.push_back(batch.fallbackReason);
			}
			plan.batches.push_back(std::move(batch));
		}
		return plan;
	}

}
