// RenderWorkPacket.h - extended render packet planning helpers.
//
// The base packet type lives in RenderGraph.h because graph validation needs
// it. This header adds backend-recording batch metadata without exposing any
// API-specific command-buffer handles to renderer-facing code.

#pragma once

#include "RenderGraph.h"
#include "Animation/Gpu/GpuAnimationPlan.h"

#include <memory>

namespace TerraEngine::Renderer {

	enum class RenderBackendWorkKind
	{
		Geometry,
		Shadow,
		UI,
		Culling,
		Descriptor,
		Upload,
		Transition,
		Validation,
		Stats
	};

	struct BackendRecordingBatch
	{
		std::string batchId;
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		std::string passId;
		RenderBackendWorkKind workKind = RenderBackendWorkKind::Geometry;
		std::size_t begin = 0u;
		std::size_t end = 0u;
		bool workerEligible = false;
		std::string fallbackReason;
	};

	struct BackendRecordingBatchPlan
	{
		std::vector<BackendRecordingBatch> batches;
		std::size_t workerBatchCount = 0u;
		std::size_t serialBatchCount = 0u;
		std::vector<std::string> serializedReasons;
	};

	// GPU animation executes once per deformation generation, before any number
	// of camera/view draw packets consume the resulting palette. This plan keeps
	// that work separate from per-view recording batches and prevents accidental
	// per-camera resubmission.
	struct AnimationBackendWorkRange
	{
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t animationPlanGeneration = 0U;
		std::uint64_t paletteGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint32_t firstGpuWorkPacket = 0U;
		std::uint32_t gpuWorkPacketCount = 0U;
		std::uint32_t gpuStageMask = 0U;
	};

	struct AnimationBackendWorkPlan
	{
		std::uint64_t frameId = 0U;
		std::vector<AnimationBackendWorkRange> ranges;
		std::size_t gpuAnimatorCount = 0U;
		std::size_t cpuOrFallbackAnimatorCount = 0U;
		// Aliasing immutable ownership keeps the exact vectors referenced by ranges
		// alive until the backend's protected submission boundary has consumed them.
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan;
		// CPU palettes use a separate immutable plan because they require no compute
		// dispatch. Vulkan still consumes them at the same protected pre-view
		// boundary, so multiple cameras cannot trigger duplicate uploads.
		std::shared_ptr<const CpuAnimationDeformationPlan> cpuDeformationPlan;
		bool valid = true;
		std::string rejectionReason;
	};

	TERRAENGINE_API const char* ToString(RenderBackendWorkKind workKind);
	TERRAENGINE_API BackendRecordingBatchPlan BuildBackendRecordingBatchPlan(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const BackendCapabilityProfile& capabilities,
		RenderBackendWorkKind workKind);
	TERRAENGINE_API AnimationBackendWorkPlan BuildAnimationBackendWorkPlan(
		const RenderFrameSnapshot& snapshot,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan = {});

}
