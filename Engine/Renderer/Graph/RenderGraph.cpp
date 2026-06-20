#include "RenderGraph.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_map>

namespace TerraEngine::Renderer {

	std::string ToString(RenderWorkPolicy policy)
	{
		switch (policy)
		{
		case RenderWorkPolicy::WorkerEligible:
			return "worker";
		case RenderWorkPolicy::SerialRequired:
			return "serial";
		case RenderWorkPolicy::FallbackOnly:
			return "fallback";
		default:
			return "unknown";
		}
	}

	std::string ToString(RenderPacketExecutionPolicy policy)
	{
		switch (policy)
		{
		case RenderPacketExecutionPolicy::Worker:
			return "worker";
		case RenderPacketExecutionPolicy::Serial:
			return "serial";
		case RenderPacketExecutionPolicy::Skipped:
			return "skipped";
		case RenderPacketExecutionPolicy::Failed:
			return "failed";
		default:
			return "unknown";
		}
	}

	bool IsWriteAccess(RenderAccessMode mode)
	{
		return mode == RenderAccessMode::Write
			|| mode == RenderAccessMode::ReadWrite
			|| mode == RenderAccessMode::Present
			|| mode == RenderAccessMode::Upload
			|| mode == RenderAccessMode::Resolve;
	}

	namespace {
		bool DependsOn(const RenderPassNode& pass, const std::string& dependency)
		{
			return std::find(pass.dependencies.begin(), pass.dependencies.end(), dependency) != pass.dependencies.end();
		}

		void AddDiagnostic(RenderGraphValidationResult& result, std::string code, std::string passId, std::string message, std::string action)
		{
			result.valid = false;
			RenderGraphValidationDiagnostic diagnostic;
			diagnostic.severity = "error";
			diagnostic.code = std::move(code);
			diagnostic.passId = std::move(passId);
			diagnostic.message = std::move(message);
			diagnostic.recommendedAction = std::move(action);
			result.diagnostics.push_back(std::move(diagnostic));
		}
	}

	RenderGraphValidationResult ValidateRenderGraph(const std::vector<RenderPassNode>& passes)
	{
		RenderGraphValidationResult result;
		std::set<std::string> passIds;
		for (const RenderPassNode& pass : passes)
		{
			if (pass.passId.empty())
			{
				AddDiagnostic(result, "missing-pass-id", pass.passId, "Render pass has no stable id.", "Assign a pass id before scheduling.");
				continue;
			}
			if (!passIds.insert(pass.passId).second)
			{
				AddDiagnostic(result, "duplicate-pass-id", pass.passId, "Render pass id is duplicated.", "Use deterministic unique ids for every pass.");
			}
			if (pass.workPolicy != RenderWorkPolicy::WorkerEligible && pass.fallbackReason.empty())
			{
				AddDiagnostic(result, "missing-serial-reason", pass.passId, "Serial or fallback pass has no reason.", "Explain the ownership or capability boundary.");
			}
		}

		for (std::size_t leftIndex = 0u; leftIndex < passes.size(); ++leftIndex)
		{
			for (std::size_t rightIndex = leftIndex + 1u; rightIndex < passes.size(); ++rightIndex)
			{
				const RenderPassNode& left = passes[leftIndex];
				const RenderPassNode& right = passes[rightIndex];
				for (const RenderResourceState& leftOutput : left.outputs)
				{
					for (const RenderResourceState& rightInput : right.inputs)
					{
						if (leftOutput.resourceId == 0u || leftOutput.resourceId != rightInput.resourceId)
						{
							continue;
						}
						if (IsWriteAccess(leftOutput.accessMode) && !DependsOn(right, left.passId))
						{
							AddDiagnostic(result, "unordered-read-write", right.passId, "Pass reads a resource written by an earlier pass without a dependency.", "Add an explicit pass dependency or split the resource.");
						}
					}
					for (const RenderResourceState& rightOutput : right.outputs)
					{
						if (leftOutput.resourceId == 0u || leftOutput.resourceId != rightOutput.resourceId)
						{
							continue;
						}
						if ((IsWriteAccess(leftOutput.accessMode) || IsWriteAccess(rightOutput.accessMode)) && !DependsOn(right, left.passId))
						{
							AddDiagnostic(result, "unordered-write-conflict", right.passId, "Pass writes a resource that conflicts with another writer.", "Serialize the passes with graph dependencies.");
						}
					}
				}
			}
		}

		return result;
	}

	std::vector<RenderPassNode> BuildDefaultRenderPasses(const RenderFrameSnapshot& snapshot, const BackendCapabilityProfile& capabilities)
	{
		std::vector<RenderPassNode> passes;
		const bool workerAllowed = capabilities.supportsParallelRecording && capabilities.maxRecordingWorkers > 1u;
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			RenderPassNode collectPass;
			collectPass.passId = "view-" + std::to_string(view.outputId) + ".collect";
			collectPass.viewType = view.viewType;
			collectPass.workPolicy = workerAllowed ? RenderWorkPolicy::WorkerEligible : RenderWorkPolicy::SerialRequired;
			collectPass.fallbackReason = workerAllowed ? "" : "Backend capability profile disables parallel recording.";
			collectPass.profilingLabel = "RenderGraph: Collect View";
			collectPass.outputs.push_back({ view.outputId + 1u, RenderAccessMode::Write, RenderStage::Cull, collectPass.passId });
			passes.push_back(std::move(collectPass));

			RenderPassNode drawPass;
			drawPass.passId = "view-" + std::to_string(view.outputId) + ".draw";
			drawPass.viewType = view.viewType;
			drawPass.dependencies.push_back(passes.back().passId);
			drawPass.workPolicy = workerAllowed ? RenderWorkPolicy::WorkerEligible : RenderWorkPolicy::SerialRequired;
			drawPass.fallbackReason = workerAllowed ? "" : "Backend capability profile disables parallel recording.";
			drawPass.profilingLabel = "RenderGraph: Draw View";
			drawPass.inputs.push_back({ view.outputId + 1u, RenderAccessMode::Read, RenderStage::Geometry, drawPass.passId });
			drawPass.outputs.push_back({ view.outputId + 100u, RenderAccessMode::Write, RenderStage::Geometry, drawPass.passId });
			passes.push_back(std::move(drawPass));
		}
		return passes;
	}

	RenderWorkPacketPlan BuildRenderWorkPacketPlan(const RenderFrameSnapshot& snapshot, const std::vector<RenderPassNode>& passes, const BackendCapabilityProfile& capabilities)
	{
		RenderWorkPacketPlan plan;
		const std::size_t workload = static_cast<std::size_t>(std::max<std::uint64_t>(1u, EstimateSnapshotWorkload(snapshot)));
		const std::size_t workerLimit = std::max<std::size_t>(1u, capabilities.maxRecordingWorkers);
		const bool preferWorkers = capabilities.supportsParallelRecording && workload >= capabilities.workerSchedulingThreshold && workerLimit > 1u;
		const std::size_t packetCount = preferWorkers ? std::min(workerLimit, workload) : 1u;
		const std::size_t chunkSize = (workload + packetCount - 1u) / packetCount;

		for (const RenderPassNode& pass : passes)
		{
			const bool passCanUseWorkers = preferWorkers && pass.workPolicy == RenderWorkPolicy::WorkerEligible;
			if (!passCanUseWorkers)
			{
				if (pass.workPolicy != RenderWorkPolicy::WorkerEligible || !capabilities.supportsParallelRecording)
				{
					plan.serializedReasons.push_back(pass.fallbackReason.empty() ? "Pass requires coordinator ownership." : pass.fallbackReason);
				}
			}

			for (std::size_t packetIndex = 0u; packetIndex < packetCount; ++packetIndex)
			{
				const std::size_t begin = packetIndex * chunkSize;
				if (begin >= workload)
				{
					break;
				}
				const std::size_t end = std::min(workload, begin + chunkSize);
				RenderWorkPacket packet;
				packet.packetId = pass.passId + "." + std::to_string(packetIndex);
				packet.passId = pass.passId;
				packet.begin = begin;
				packet.end = end;
				packet.executionPolicy = passCanUseWorkers ? RenderPacketExecutionPolicy::Worker : RenderPacketExecutionPolicy::Serial;
				packet.inputs = pass.inputs;
				packet.outputs = pass.outputs;
				packet.profilingLabel = pass.profilingLabel;
				plan.workerPacketCount += packet.executionPolicy == RenderPacketExecutionPolicy::Worker ? 1u : 0u;
				plan.serialPacketCount += packet.executionPolicy == RenderPacketExecutionPolicy::Serial ? 1u : 0u;
				plan.packets.push_back(std::move(packet));
			}
		}

		return plan;
	}

}
