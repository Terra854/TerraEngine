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

	enum class RenderFailurePolicy
	{
		Fatal,
		Degrade,
		Skip,
		Retry,
		DiagnosticsOnly
	};

	enum class RenderWorkerOwnership
	{
		WorkerSafe,
		CoordinatorOnly
	};

	enum class RenderWorkBudgetCategory
	{
		Upload,
		DrawPreparation,
		ShadowPreparation,
		UiPreparation,
		PipelinePreparation,
		Readback,
		TransientMemory,
		WorkerScratchMemory,
		DeferredRetirement
	};

	enum class RenderBudgetOverflowPolicy
	{
		Delay,
		Cancel,
		Degrade,
		Serialize,
		Reject
	};

	enum class RenderQueueCategory
	{
		Transfer,
		Compute,
		Graphics,
		Presentation
	};

	enum class RenderQueueMode
	{
		Parallel,
		Serial,
		Unavailable,
		Fallback
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
		Running,
		Completed,
		Cancelled,
		Stale,
		Failed,
		Degraded
	};

	struct RenderWorkBudget
	{
		// Budgets describe how much optional work a frame may start before it
		// should delay, cancel, or degrade that work. The renderer uses this to
		// keep streaming and preparation tasks from stealing visible frame time.
		RenderWorkBudgetCategory category = RenderWorkBudgetCategory::DrawPreparation;
		std::size_t limit = 0u;
		std::size_t currentUsage = 0u;
		RenderBudgetOverflowPolicy overflowPolicy = RenderBudgetOverflowPolicy::Delay;
		std::string diagnosticLabel;
	};

	struct RenderQueueReadiness
	{
		// Queue readiness is backend-neutral. Vulkan, DirectX 12, and future
		// APIs can report different transfer/compute/graphics capabilities while
		// the high-level renderer still reasons about the same categories.
		RenderQueueCategory category = RenderQueueCategory::Graphics;
		RenderQueueMode mode = RenderQueueMode::Serial;
		bool handoffRequired = false;
		std::string fallbackReason;
	};

	struct RenderCancellationToken
	{
		// Worker results are accepted only when their owner revision and frame
		// still match the current renderer state. This prevents old background
		// work from publishing data after the scene or resources changed.
		std::uint64_t ownerId = 0u;
		std::uint64_t ownerRevision = 0u;
		std::uint64_t frameId = 0u;
		std::string reason;
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
		// A pass node describes data dependencies, not backend commands. Worker
		// passes may prepare immutable CPU-side data, while coordinator-owned
		// passes cover present, swapchain, recovery, and final submission work.
		std::string passId;
		RenderViewType viewType = RenderViewType::Game;
		std::vector<RenderResourceState> inputs;
		std::vector<RenderResourceState> outputs;
		std::vector<std::string> dependencies;
		RenderWorkPolicy workPolicy = RenderWorkPolicy::WorkerEligible;
		RenderFailurePolicy failurePolicy = RenderFailurePolicy::Fatal;
		RenderWorkerOwnership ownership = RenderWorkerOwnership::WorkerSafe;
		bool canOverlap = true;
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
		RenderFailurePolicy failurePolicy = RenderFailurePolicy::Fatal;
		RenderCancellationToken cancellationToken;
		std::string profilingLabel;
	};

	struct PreparedDrawPacketSignature
	{
		// The signature is a compact "did this draw input change?" key. It lets
		// stable objects reuse prepared draw packets without comparing full
		// renderer records every frame.
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		std::uint64_t transformHash = 0u;
		std::uint64_t visibilityHash = 0u;
		std::uint64_t overrideHash = 0u;
		// Virtual geometry LOD selection changes which meshlet range the backend
		// dispatches, so it must invalidate prepared draw packets just like a
		// material or transform change.
		std::uint64_t virtualGeometryHash = 0u;
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
	TERRAENGINE_API std::string ToString(RenderFailurePolicy policy);
	TERRAENGINE_API std::string ToString(RenderWorkBudgetCategory category);
	TERRAENGINE_API std::string ToString(RenderBudgetOverflowPolicy policy);
	TERRAENGINE_API std::string ToString(RenderQueueCategory category);
	TERRAENGINE_API std::string ToString(RenderQueueMode mode);
	TERRAENGINE_API bool IsWriteAccess(RenderAccessMode mode);
	TERRAENGINE_API bool IsCoordinatorOwnedAccess(RenderAccessMode mode);
	TERRAENGINE_API bool IsRenderWorkOverBudget(const RenderWorkBudget& budget);
	TERRAENGINE_API bool IsCancellationTokenCurrent(const RenderCancellationToken& token, std::uint64_t ownerRevision, std::uint64_t frameId);
	TERRAENGINE_API bool IsRenderWorkPacketStale(const RenderWorkPacket& packet, std::uint64_t ownerRevision, std::uint64_t frameId);
	TERRAENGINE_API PreparedDrawPacketSignature BuildPreparedDrawPacketSignature(const RenderFrameRenderableRecord& record);
	TERRAENGINE_API std::uint64_t HashPreparedDrawPacketSignature(const PreparedDrawPacketSignature& signature);
	TERRAENGINE_API RenderGraphValidationResult ValidateRenderGraph(const std::vector<RenderPassNode>& passes);
	TERRAENGINE_API std::vector<RenderPassNode> BuildDefaultRenderPasses(const RenderFrameSnapshot& snapshot, const BackendCapabilityProfile& capabilities);
	TERRAENGINE_API RenderWorkPacketPlan BuildRenderWorkPacketPlan(const RenderFrameSnapshot& snapshot, const std::vector<RenderPassNode>& passes, const BackendCapabilityProfile& capabilities);

}
