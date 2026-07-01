#include "RenderGraph.h"

#include <algorithm>
#include <cstring>
#include <functional>
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

	std::string ToString(RenderFailurePolicy policy)
	{
		switch (policy)
		{
		case RenderFailurePolicy::Fatal: return "fatal";
		case RenderFailurePolicy::Degrade: return "degrade";
		case RenderFailurePolicy::Skip: return "skip";
		case RenderFailurePolicy::Retry: return "retry";
		case RenderFailurePolicy::DiagnosticsOnly: return "diagnostics-only";
		default: return "unknown";
		}
	}

	std::string ToString(RenderWorkBudgetCategory category)
	{
		switch (category)
		{
		case RenderWorkBudgetCategory::Upload: return "upload";
		case RenderWorkBudgetCategory::DrawPreparation: return "draw-preparation";
		case RenderWorkBudgetCategory::ShadowPreparation: return "shadow-preparation";
		case RenderWorkBudgetCategory::UiPreparation: return "ui-preparation";
		case RenderWorkBudgetCategory::PipelinePreparation: return "pipeline-preparation";
		case RenderWorkBudgetCategory::Readback: return "readback";
		case RenderWorkBudgetCategory::TransientMemory: return "transient-memory";
		case RenderWorkBudgetCategory::WorkerScratchMemory: return "worker-scratch-memory";
		case RenderWorkBudgetCategory::DeferredRetirement: return "deferred-retirement";
		default: return "unknown";
		}
	}

	std::string ToString(RenderBudgetOverflowPolicy policy)
	{
		switch (policy)
		{
		case RenderBudgetOverflowPolicy::Delay: return "delay";
		case RenderBudgetOverflowPolicy::Cancel: return "cancel";
		case RenderBudgetOverflowPolicy::Degrade: return "degrade";
		case RenderBudgetOverflowPolicy::Serialize: return "serialize";
		case RenderBudgetOverflowPolicy::Reject: return "reject";
		default: return "unknown";
		}
	}

	std::string ToString(RenderQueueCategory category)
	{
		switch (category)
		{
		case RenderQueueCategory::Transfer: return "transfer";
		case RenderQueueCategory::Compute: return "compute";
		case RenderQueueCategory::Graphics: return "graphics";
		case RenderQueueCategory::Presentation: return "presentation";
		default: return "unknown";
		}
	}

	std::string ToString(RenderQueueMode mode)
	{
		switch (mode)
		{
		case RenderQueueMode::Parallel: return "parallel";
		case RenderQueueMode::Serial: return "serial";
		case RenderQueueMode::Unavailable: return "unavailable";
		case RenderQueueMode::Fallback: return "fallback";
		default: return "unknown";
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

	bool IsCoordinatorOwnedAccess(RenderAccessMode mode)
	{
		return mode == RenderAccessMode::Present;
	}

	bool IsRenderWorkOverBudget(const RenderWorkBudget& budget)
	{
		return budget.limit > 0u && budget.currentUsage > budget.limit;
	}

	bool IsCancellationTokenCurrent(const RenderCancellationToken& token, std::uint64_t ownerRevision, std::uint64_t frameId)
	{
		return token.ownerRevision == ownerRevision && token.frameId == frameId;
	}

	bool IsRenderWorkPacketStale(const RenderWorkPacket& packet, std::uint64_t ownerRevision, std::uint64_t frameId)
	{
		if (packet.state == RenderPacketState::Stale || packet.state == RenderPacketState::Cancelled)
		{
			return true;
		}
		return !IsCancellationTokenCurrent(packet.cancellationToken, ownerRevision, frameId);
	}

	namespace {
		void HashCombine(std::uint64_t& seed, std::uint64_t value)
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
		}

		std::uint64_t HashUuid(const TerraEngine::UUID& id)
		{
			return static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(id));
		}

		std::uint64_t HashMatrix(const glm::mat4& matrix)
		{
			std::uint64_t hash = 1469598103934665603ull;
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					std::uint32_t bits = 0u;
					const float value = matrix[column][row];
					std::memcpy(&bits, &value, sizeof(float));
					HashCombine(hash, bits);
				}
			}
			return hash;
		}

		std::uint64_t HashVirtualGeometryRange(const VirtualGeometryDrawRange& range)
		{
			std::uint64_t hash = 1469598103934665603ull;
			HashCombine(hash, range.selectedLodIndex);
			HashCombine(hash, range.meshletOffset);
			HashCombine(hash, range.meshletCount);
			HashCombine(hash, range.virtualGeometryActive ? 1u : 0u);
			HashCombine(hash, range.failOpen ? 1u : 0u);
			HashCombine(hash, range.hiZSelfOcclusionGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZNearPlaneGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZThinGeometryGuardPassed ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(range.hiZInactiveReason)));
			HashCombine(hash, range.selectedClusterOffset);
			HashCombine(hash, range.selectedClusterCount);
			HashCombine(hash, range.pageNeedOffset);
			HashCombine(hash, range.pageNeedCount);
			return hash;
		}
	}

	PreparedDrawPacketSignature BuildPreparedDrawPacketSignature(const RenderFrameRenderableRecord& record)
	{
		PreparedDrawPacketSignature signature;
		signature.objectId = record.objectId;
		signature.meshId = record.meshId;
		signature.materialId = record.materialId;
		signature.transformHash = HashMatrix(record.modelMatrix);
		signature.visibilityHash = record.taskShaderCullingEnabled ? 1u : 0u;
		signature.overrideHash = record.usingFallback ? 1u : 0u;
		signature.virtualGeometryHash = HashVirtualGeometryRange(record.virtualGeometry);
		return signature;
	}

	std::uint64_t HashPreparedDrawPacketSignature(const PreparedDrawPacketSignature& signature)
	{
		std::uint64_t hash = HashUuid(signature.objectId);
		HashCombine(hash, HashUuid(signature.meshId));
		HashCombine(hash, HashUuid(signature.materialId));
		HashCombine(hash, signature.transformHash);
		HashCombine(hash, signature.visibilityHash);
		HashCombine(hash, signature.overrideHash);
		HashCombine(hash, signature.virtualGeometryHash);
		return hash;
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
			if (pass.ownership == RenderWorkerOwnership::CoordinatorOnly && pass.workPolicy == RenderWorkPolicy::WorkerEligible)
			{
				AddDiagnostic(result, "worker-owns-coordinator-pass", pass.passId, "Coordinator-owned pass is marked worker eligible.", "Serialize coordinator-only work.");
			}
			if (!pass.canOverlap && pass.workPolicy == RenderWorkPolicy::WorkerEligible && pass.dependencies.empty())
			{
				AddDiagnostic(result, "missing-non-overlap-dependency", pass.passId, "Non-overlapping worker pass has no ordering dependency.", "Add a dependency or mark the pass serial.");
			}
			for (const RenderResourceState& output : pass.outputs)
			{
				if (IsCoordinatorOwnedAccess(output.accessMode) && pass.ownership != RenderWorkerOwnership::CoordinatorOnly)
				{
					AddDiagnostic(result, "present-not-coordinator-owned", pass.passId, "Presentation output is not coordinator-owned.", "Keep presentation and final submit on the coordinator path.");
				}
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
				packet.failurePolicy = pass.failurePolicy;
				packet.cancellationToken = { snapshot.frameId, snapshot.sceneRevision, snapshot.frameId, "frame-snapshot" };
				packet.profilingLabel = pass.profilingLabel;
				plan.workerPacketCount += packet.executionPolicy == RenderPacketExecutionPolicy::Worker ? 1u : 0u;
				plan.serialPacketCount += packet.executionPolicy == RenderPacketExecutionPolicy::Serial ? 1u : 0u;
				plan.packets.push_back(std::move(packet));
			}
		}

		return plan;
	}

}
