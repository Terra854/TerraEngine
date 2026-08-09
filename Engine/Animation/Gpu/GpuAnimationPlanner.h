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
	// generations. They never contain backend objects, allocations, or scene data.
	struct GpuAnimationVisualInput {
		GpuAnimationGenerationRecord generations{};
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t skeletonTopologyId = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint64_t inputPoseOffsetBytes = 0U;
		// Hashes every controller parameter value that can alter sampled visual output.
		// Zero is valid only for a proven parameter-free compact program.
		std::uint64_t evaluationParameterFingerprint = 0U;
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
		// A prior-frame visibility decision may freeze presentation without removing
		// this packet. Stable packet offsets let shaders retain the last complete pose.
		bool suspendVisualEvaluation = false;
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
		// A producer may reuse a successful validation only for the exact same
		// immutable EvaluationProgram object inside one worker-owned batch. Direct
		// callers keep the checked default so untrusted programs are never accepted.
		bool programValidated = false;
		// A non-zero mask is the result of DeriveStageMask for this exact program and
		// visual feature shape. AnimationSystem fills it only after checked lowering;
		// later members of the same worker batch and presentation can then avoid
		// rescanning identical immutable instructions. Direct callers leave it zero.
		std::uint32_t prevalidatedStageMask = 0U;
		// The producer owns this shared immutable copy for the whole frame.  It is
		// deliberately not a span because lowering may outlive asset-cache changes.
		std::shared_ptr<const GpuAnimationResidentPayloads> residentPayloads;
		// AnimationSystem sets this only after validating the exact lowered request
		// against the shared immutable payload. Presentation may reuse that proof;
		// direct callers and tests retain the checked default-false path.
		bool residentPayloadsValidated = false;
		// Protected compact capture sets this after proving that no per-owner CPU
		// authority can observe the GPU evaluation payload.
		bool allowSharedVisualEvaluation = false;
	};

	// Producers already visit every lowering input while assembling the immutable
	// presentation frame. Extending this signature during that mandatory pass lets
	// the planner validate a retained topology without scanning the crowd twice.
	class TERRAENGINE_API GpuAnimationLoweringTopologySignatureBuilder {
	public:
		void Include(const GpuAnimationLoweringInput& input) noexcept;
		[[nodiscard]] std::uint64_t Complete() const noexcept;

	private:
		// Worker completion order is not a topology property. Three independent
		// commutative accumulators retain a strong 64-bit set fingerprint without
		// sorting the crowd or making the protected publisher order-dependent.
		std::uint64_t xor_ = 0U;
		std::uint64_t sum_ = 0U;
		std::uint64_t productSum_ = 0U;
		std::uint64_t count_ = 0U;
	};

	// The producer already computed topologyInputSignature while assembling the
	// immutable presentation input. An exact prior signature permits a warm frame
	// to publish only generation/tick/render-identity lanes without taking the
	// mutable atlas-cache lock. Any mismatch returns false without replacing output.
	[[nodiscard]] TERRAENGINE_API bool TryRefreshLoweredGpuAnimationDynamicPlan(
		std::span<const GpuAnimationLoweringInput> inputs,
		std::uint64_t topologyInputSignature,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		const GpuAnimationPlan& previous,
		GpuAnimationPlan& output,
		const GpuAnimationPlanLimits& limits = {},
		std::shared_ptr<const GpuAnimationPlan> previousOwner = {},
		const std::shared_ptr<GpuAnimationPlanDynamicLanes>& dynamicLaneScratch = {}) noexcept;

	struct GpuAnimationLoweringResult {
		GpuAnimationLoweringStatus status = GpuAnimationLoweringStatus::InvalidArgument;
		GpuAnimationWorkRequest request{};
		bool emitsGpuWork = false;
	};

	// One result describes a complete transactional lowering attempt. workRequests
	// is retained for diagnostics and tests after a full topology build. A warmed
	// retained-plan refresh leaves it empty because production has already validated
	// and published the requests without materializing a duplicate diagnostic vector.
	struct GpuAnimationLoweredPlanResult {
		GpuAnimationLoweringStatus status = GpuAnimationLoweringStatus::InvalidArgument;
		GpuAnimationPlanStatus planStatus = GpuAnimationPlanStatus::InvalidArgument;
		std::vector<GpuAnimationWorkRequest> workRequests;
		GpuAnimationPlanBuildDiagnostics diagnostics{};
		bool topologyReused = false;
	};

	// Each entry couples one already-lowered request to the immutable descriptor
	// payload whose absolute offsets it references. Atlas construction stages all
	// bytes and remaps every offset before publishing, so failure leaves output
	// untouched and never exposes a partially relocated frame.
	struct GpuAnimationFrameAtlasEntry {
		GpuAnimationWorkRequest request{};
		std::shared_ptr<const GpuAnimationResidentPayloads> residentPayloads;
		bool residentPayloadsValidated = false;
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

	// Presentation rebuilds dynamic work requests every frame, but the immutable
	// clip/program/skeleton atlas changes only when its source payload owners or
	// selected resource shapes change. This cache retains one checked relocation
	// layout without owning the source assets; callers provide their own external
	// synchronization when preparation can overlap.
	struct GpuAnimationFrameAtlasCacheEntry {
		std::weak_ptr<const GpuAnimationResidentPayloads> sourcePayload;
		std::uint32_t sourceProgramResourceIndex = 0U;
		std::uint32_t sourceSkeletonResourceIndex = 0U;
		std::uint32_t sourceMorphResourceIndex = 0U;
		std::uint32_t jointCount = 0U;
		std::uint32_t morphCount = 0U;
		std::uint32_t paletteEntryCount = 0U;
		std::uint32_t relocatedClipResourceIndex = 0U;
		std::uint32_t relocatedProgramResourceIndex = 0U;
		std::uint32_t relocatedSkeletonResourceIndex = 0U;
		std::uint32_t relocatedMorphResourceIndex = 0U;
	};

	struct GpuAnimationFrameAtlasCache {
		std::vector<GpuAnimationFrameAtlasCacheEntry> entries;
		std::shared_ptr<const GpuAnimationResidentPayloads> residentPayloads;

		void Clear() noexcept
		{
			entries.clear();
			residentPayloads.reset();
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
		const GpuAnimationPlanLimits& limits = {},
		GpuAnimationFrameAtlasCache* atlasCache = nullptr,
		const GpuAnimationPlan* previousPlan = nullptr,
		std::shared_ptr<const GpuAnimationPlan> previousPlanOwner = {},
		std::uint64_t precomputedTopologyInputSignature = 0U,
		const std::shared_ptr<GpuAnimationPlanDynamicLanes>& dynamicLaneScratch = {}) noexcept;
}
