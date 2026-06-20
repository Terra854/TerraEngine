// RenderGraph.h - backend-neutral render pass and work packet contracts.
//
// High-level renderer code speaks in passes, resources, and packets. Backend
// command buffers, image layouts, queues, and fences stay below RHIContext so a
// future graphics API can consume the same graph without changing scene data.

#pragma once

#include "RenderFrameSnapshot.h"
#include "Renderer/RHI/RHIDefinitions.h"
#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	enum class RenderResourceKind
	{
		Texture,
		Buffer,
		Target,
		Transient,
		SwapchainTarget
	};

	enum class RenderResourceLifetime
	{
		Imported,
		Persistent,
		Transient,
		Retired
	};

	enum class RenderAccessMode
	{
		Read,
		Write,
		ReadWrite,
		Present,
		Upload,
		Resolve
	};

	enum class RenderStage
	{
		Unknown,
		Upload,
		Cull,
		Shadow,
		Geometry,
		Lighting,
		UI,
		Present
	};

	enum class RenderWorkPolicy
	{
		WorkerEligible,
		SerialRequired,
		FallbackOnly
	};

	enum class RenderPacketExecutionPolicy
	{
		Worker,
		Serial,
		Skipped,
		Failed
	};

	enum class RenderPacketState
	{
		Pending,
		Completed,
		Cancelled,
		Failed
	};

	struct RenderResourceHandle
	{
		std::uint64_t resourceId = 0u;
		RenderResourceKind resourceKind = RenderResourceKind::Texture;
		std::uint64_t ownerFrameId = 0u;
		RenderResourceLifetime lifetime = RenderResourceLifetime::Persistent;
		std::string debugName;
	};

	struct RenderResourceState
	{
		std::uint64_t resourceId = 0u;
		RenderAccessMode accessMode = RenderAccessMode::Read;
		RenderStage stage = RenderStage::Unknown;
		std::string ownerPassId;
	};

	struct RenderPassNode
	{
		std::string passId;
		RenderViewType viewType = RenderViewType::Game;
		std::vector<RenderResourceState> inputs;
		std::vector<RenderResourceState> outputs;
		std::vector<std::string> dependencies;
		RenderWorkPolicy workPolicy = RenderWorkPolicy::WorkerEligible;
		std::string fallbackReason;
		std::string profilingLabel;
	};

	struct RenderWorkPacket
	{
		std::string packetId;
		std::string passId;
		std::size_t begin = 0u;
		std::size_t end = 0u;
		RenderPacketExecutionPolicy executionPolicy = RenderPacketExecutionPolicy::Worker;
		std::vector<RenderResourceState> inputs;
		std::vector<RenderResourceState> outputs;
		std::vector<std::string> diagnostics;
		RenderPacketState state = RenderPacketState::Pending;
		std::string profilingLabel;
	};

	struct RenderGraphValidationDiagnostic
	{
		std::string severity = "info";
		std::string code;
		std::string passId;
		std::string message;
		std::string recommendedAction;
	};

	struct RenderGraphValidationResult
	{
		bool valid = true;
		std::vector<RenderGraphValidationDiagnostic> diagnostics;
	};

	struct RenderWorkPacketPlan
	{
		std::vector<RenderWorkPacket> packets;
		std::size_t workerPacketCount = 0u;
		std::size_t serialPacketCount = 0u;
		std::size_t skippedPacketCount = 0u;
		std::vector<std::string> serializedReasons;
	};

	TERRAENGINE_API std::string ToString(RenderWorkPolicy policy);
	TERRAENGINE_API std::string ToString(RenderPacketExecutionPolicy policy);
	TERRAENGINE_API bool IsWriteAccess(RenderAccessMode mode);
	TERRAENGINE_API RenderGraphValidationResult ValidateRenderGraph(const std::vector<RenderPassNode>& passes);
	TERRAENGINE_API std::vector<RenderPassNode> BuildDefaultRenderPasses(const RenderFrameSnapshot& snapshot, const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API RenderWorkPacketPlan BuildRenderWorkPacketPlan(const RenderFrameSnapshot& snapshot, const std::vector<RenderPassNode>& passes, const BackendCapabilityProfile& capabilities);

}
