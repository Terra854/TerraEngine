// AnimatedVirtualGeometryRuntime.h - renderer-neutral validation and publication.
//
// The Vulkan backend consumes these immutable value records at its protected
// command-recording boundary. Keeping arithmetic and acceptance rules free of
// Vulkan handles makes them deterministic, worker-safe, and directly testable.

#pragma once

#include "Core/Export.h"
#include "Renderer/Deformation/GpuDeformationPlan.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	struct CpuDeformationVertex
	{
		std::array<float, 3> position{};
		std::array<float, 3> normal{ 0.0f, 1.0f, 0.0f };
		std::array<float, 3> tangent{ 1.0f, 0.0f, 0.0f };
		float tangentSign = 1.0f;
	};

	struct CpuMorphDelta
	{
		std::array<float, 3> position{};
		std::array<float, 3> normal{};
		std::array<float, 3> tangent{};
	};

	struct CpuSkinRecord
	{
		std::array<std::uint8_t, 4> joints{};
		std::array<std::uint8_t, 4> weights{};
	};

	// Three affine rows mirror the compact palette representation consumed by
	// AnimatedPageDeform.comp.hlsl. Translation occupies lane three.
	struct CpuAffinePalette
	{
		std::array<std::array<float, 4>, 3> rows{};
	};

	struct CpuDeformationResult
	{
		GpuDeformedVertexRecord vertex{};
		bool valid = false;
		std::string rejectionReason;
	};

	TERRAENGINE_API CpuDeformationResult EvaluateAnimatedPageVertexReference(
		const CpuDeformationVertex& source,
		std::span<const CpuMorphDelta> targetMajorDeltas,
		std::span<const float> morphWeights,
		const CpuSkinRecord& skin,
		std::span<const CpuAffinePalette> palette,
		const GpuDeformedVertexRecord* compatibleHistory);

	enum class DynamicBoundsProof : std::uint8_t
	{
		Missing,
		Current,
		Stale,
		NonFinite,
		Conflicting
	};

	struct DynamicBoundsResult
	{
		GpuDynamicBoundsRecord bounds{};
		DynamicBoundsProof proof = DynamicBoundsProof::Missing;
		bool valid = false;
		std::string rejectionReason;
	};

	TERRAENGINE_API DynamicBoundsResult ReduceAnimatedPageBounds(
		std::span<const GpuDeformedVertexRecord> vertices,
		std::array<std::uint32_t, 4> identity);

	struct DeformationWorkingSetBudget
	{
		std::uint64_t currentBytes = 0u;
		std::uint64_t historyBytes = 0u;
		std::uint64_t boundsBytes = 0u;
	};

	struct DeformationWorkingSetPlan
	{
		DeformationWorkingSetBudget requested{};
		DeformationWorkingSetBudget committed{};
		std::uint64_t limitBytes = 0u;
		std::uint64_t totalBytes = 0u;
		bool accepted = false;
		std::string rejectionReason;
	};

	TERRAENGINE_API DeformationWorkingSetPlan PlanDeformationWorkingSet(
		const GpuDeformationPlan& plan,
		std::uint64_t limitBytes);

	struct DeformedPagePublication
	{
		std::uint64_t publicationGeneration = 0u;
		DeformedPageKey key{};
		std::uint64_t staticAllocationId = 0u;
		std::uint64_t currentAllocationId = 0u;
		std::uint64_t previousAllocationId = 0u;
		std::uint64_t boundsAllocationId = 0u;
		std::uint64_t completionFence = 0u;
		bool historyValid = false;
		bool geometryValid = false;
		bool boundsValid = false;
		bool refinementValid = false;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct DeformedPagePublicationResult
	{
		DeformedPagePublication visible{};
		bool accepted = false;
		std::string rejectionReason;
	};

	// Validate a private candidate as one atomic bundle. A rejected candidate
	// returns the prior row byte-for-byte, preventing mixed generations.
	TERRAENGINE_API DeformedPagePublicationResult PublishDeformedPageAtomically(
		const DeformedPagePublication& prior,
		const DeformedPagePublication& candidate);

	enum class AnimatedGeometryFallback : std::uint8_t
	{
		None,
		CompatibleAncestor,
		PinnedFallback,
		PriorPublication,
		LegacyOwner,
		MissingPage,
		CorruptPage,
		StaleGeneration,
		BudgetPressure,
		ShaderContract,
		DeviceRecovery
	};

	struct AnimatedGeometryDiagnostic
	{
		std::uint64_t stableInstanceId = 0u;
		std::uint32_t pageId = kInvalidDeformationRecordIndex;
		std::uint32_t viewId = 0u;
		std::uint64_t relevantGeneration = 0u;
		std::uint64_t staticBytes = 0u;
		std::uint64_t currentBytes = 0u;
		std::uint64_t historyBytes = 0u;
		std::uint64_t boundsBytes = 0u;
		AnimatedGeometryFallback fallback = AnimatedGeometryFallback::None;
		DynamicBoundsProof boundsProof = DynamicBoundsProof::Missing;
		bool cacheReused = false;
		bool shaderContractValid = true;
		std::string detail;
	};

	struct AnimatedGeometryDiagnosticSnapshot
	{
		std::vector<AnimatedGeometryDiagnostic> details;
		std::uint32_t emitted = 0u;
		std::uint32_t coalesced = 0u;
		std::uint32_t dropped = 0u;
	};

	TERRAENGINE_API AnimatedGeometryDiagnosticSnapshot CoalesceAnimatedGeometryDiagnostics(
		std::span<const AnimatedGeometryDiagnostic> diagnostics,
		std::size_t capacity = 64u);

}
