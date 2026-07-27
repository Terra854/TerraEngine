// GpuAnimationPlanner.h - Immutable evaluation-to-GPU work lowering contract.
//
// This planner deliberately accepts only copied value records and immutable
// programs. It does not know about scene objects, renderer handles, or resource
// ownership, which keeps worker preparation deterministic and render-API-neutral.

#pragma once

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Animation/Graph/EvaluationProgram.h"
#include "Animation/Runtime/AnimationExecutionMode.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	// These statuses distinguish an expected CPU-only decision from malformed GPU
	// input. A CPU fallback is successful lowering with no emitted GPU work.
	enum class GpuAnimationLoweringStatus : std::uint32_t {
		Ok = 0U,
		CpuFallbackNoGpuWork = 1U,
		InvalidArgument = 2U,
		IncompatibleProgram = 3U,
		StaleGeneration = 4U,
		PlanBuildFailed = 5U,
		AllocationFailure = 6U
	};

	// Program identity and descriptor-table position are copied while the program
	// itself remains shared immutable data. programContentId must name the exact
	// cooked program bytes used by the renderer, never a CPU pointer.
	struct GpuAnimationProgramInput {
		std::shared_ptr<const EvaluationProgram> program;
		std::uint64_t programContentId = 0U;
		std::uint32_t programResourceIndex = kGpuAnimationInvalidResourceIndex;
	};

	// Visual values identify already-resident resource-table entries and producer
	// generations. They never contain Vulkan objects, allocations, or scene data.
	struct GpuAnimationVisualInput {
		GpuAnimationGenerationRecord generations{};
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t skeletonTopologyId = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint64_t inputPoseOffsetBytes = 0U;
		std::uint32_t stableSubmissionOrdinal = 0U;
		std::uint32_t clipResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t skeletonResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t maskResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t morphResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t jointCount = 0U;
		std::uint32_t morphCount = 0U;
		std::uint32_t paletteEntryCount = 0U;
		std::uint32_t palettePartitionCount = 0U;
		std::uint32_t diagnosticIndex = 0U;
		std::uint32_t sampleTick = 0U;
		GpuAnimationRenderIdentity renderIdentity{};
		bool looping = false;
		bool reverse = false;
		bool useBindPoseFallback = false;
		bool useHoldLastFallback = false;
		bool hasRootMotion = false;
		// Optional visual stages are enabled only after their immutable table has
		// been packed for this exact frame. Merely compiling a graph node is not
		// enough to authorize a shader to consume absent runtime inputs.
		bool allowVisualProcedural = false;
		bool allowVisualRagdoll = false;
		std::uint32_t ragdollTableOffsetBytes = 0U;
	};

	// One input combines the independently immutable controller, visual packet,
	// and already-resolved effective mode. The planner never reruns policy logic.
	struct GpuAnimationLoweringInput {
		GpuAnimationProgramInput program;
		GpuAnimationVisualInput visual;
		AnimationExecutionModeDecision execution;
		// The producer owns this shared immutable copy for the whole frame.  It is
		// deliberately not a span because lowering may outlive asset-cache changes.
		std::shared_ptr<const GpuAnimationResidentPayloads> residentPayloads;
	};

	struct GpuAnimationLoweringResult {
		GpuAnimationLoweringStatus status = GpuAnimationLoweringStatus::InvalidArgument;
		GpuAnimationWorkRequest request{};
		bool emitsGpuWork = false;
	};

	// One result describes a complete transactional lowering attempt. workRequests
	// is retained for diagnostics and tests; it is empty when every animator used
	// the mandatory CPU deterministic/fallback path.
	struct GpuAnimationLoweredPlanResult {
		GpuAnimationLoweringStatus status = GpuAnimationLoweringStatus::InvalidArgument;
		GpuAnimationPlanStatus planStatus = GpuAnimationPlanStatus::InvalidArgument;
		std::vector<GpuAnimationWorkRequest> workRequests;
		GpuAnimationPlanBuildDiagnostics diagnostics{};
	};

	// Each entry couples one already-lowered request to the immutable descriptor
	// payload whose absolute offsets it references. Atlas construction stages all
	// bytes and remaps every offset before publishing, so failure leaves output
	// untouched and never exposes a partially relocated frame.
	struct GpuAnimationFrameAtlasEntry {
		GpuAnimationWorkRequest request{};
		std::shared_ptr<const GpuAnimationResidentPayloads> residentPayloads;
	};

	struct GpuAnimationFrameAtlas {
		GpuAnimationResidentPayloads residentPayloads{};
		std::vector<GpuAnimationWorkRequest> requests;

		void Clear() noexcept
		{
			residentPayloads = {};
			requests.clear();
		}
	};

	// Converts one immutable input to a checked request. CPU fallback decisions
	// return CpuFallbackNoGpuWork and leave output request untouched.
	[[nodiscard]] TERRAENGINE_API GpuAnimationLoweringStatus LowerGpuAnimationWorkRequest(
		const GpuAnimationLoweringInput& input,
		GpuAnimationWorkRequest& output) noexcept;

	// Applies the same strict descriptor-payload checks used by presentation.
	// Producers call this before committing to a GPU-only mode so malformed
	// morph, mask, procedural, or ragdoll data still receives a CPU pose.
	[[nodiscard]] TERRAENGINE_API bool ValidateGpuAnimationResidentPayloads(
		const GpuAnimationWorkRequest& request,
		const GpuAnimationResidentPayloads& payloads) noexcept;

	// Merges independently owned payloads into one aligned frame atlas. Requests
	// are returned in input order; BuildGpuAnimationPlan performs its deterministic
	// packet sort while retaining the matching CPU-only render identity.
	[[nodiscard]] TERRAENGINE_API GpuAnimationLoweringStatus BuildGpuAnimationFrameAtlas(
		std::span<const GpuAnimationFrameAtlasEntry> entries,
		GpuAnimationFrameAtlas& output) noexcept;

	// Lowers every input before invoking the transactional plan builder. Therefore
	// a bad item cannot overwrite output or advance either arena to a partial plan.
	[[nodiscard]] TERRAENGINE_API GpuAnimationLoweringStatus BuildLoweredGpuAnimationPlan(
		std::span<const GpuAnimationLoweringInput> inputs,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		GpuAnimationPlan& output,
		GpuAnimationLoweredPlanResult& result,
		const GpuAnimationPlanLimits& limits = {}) noexcept;
}
