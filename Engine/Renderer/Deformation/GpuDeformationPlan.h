// GpuDeformationPlan.h - API-neutral animated page deformation planning.
//
// Animation systems publish pose/palette authority. The renderer then creates
// one immutable plan from every view's page demand so backends can deform each
// compatible instance/page/generation exactly once.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	inline constexpr std::uint32_t kAnimatedGeometryAbiVersion = 1u;
	inline constexpr std::uint32_t kInvalidDeformationRecordIndex = 0xffffffffu;

	enum class DeformationAuthority : std::uint32_t
	{
		Cpu = 0u,
		Gpu = 1u,
		Hybrid = 2u
	};

	struct DeformationGeneration
	{
		std::uint64_t stableInstanceId = 0u;
		std::uint64_t objectGeneration = 0u;
		std::uint64_t animationPublicationGeneration = 0u;
		std::uint64_t skeletonGeneration = 0u;
		std::uint64_t morphGeneration = 0u;
		std::uint64_t meshPayloadGeneration = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t sceneGeneration = 0u;
		std::uint64_t contentIdLow = 0u;
		std::uint64_t contentIdHigh = 0u;

		[[nodiscard]] bool IsComplete() const noexcept;
		auto operator<=>(const DeformationGeneration&) const = default;
	};

	struct StaticPageKey
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint64_t meshPayloadGeneration = 0u;
		std::uint32_t pageId = kInvalidDeformationRecordIndex;
		std::uint64_t pageContentGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint32_t layoutVersion = 0u;
		// Codec identity is part of the immutable source-page key. A recook that
		// changes either field must never reuse an older deformation output.
		std::uint32_t codecProfile = 0u;
		std::uint32_t codecVersion = 0u;

		[[nodiscard]] bool IsComplete() const noexcept;
		auto operator<=>(const StaticPageKey&) const = default;
	};

	struct DeformedPageKey
	{
		StaticPageKey page;
		DeformationGeneration deformation;
		std::uint32_t inputLayoutVersion = 0u;
		// Animation LOD is independent of geometry LOD. Compatible views merge
		// only when both this level and its policy version match.
		std::uint32_t animationLod = 0u;
		std::uint32_t animationLodVersion = 0u;

		[[nodiscard]] bool IsComplete() const noexcept;
		auto operator<=>(const DeformedPageKey&) const = default;
	};

	struct DeformationPageDemand
	{
		DeformedPageKey key;
		// The owner remains instance-local even when key content is reusable. View
		// selection and the graphics bridge use this id to alias the shared output.
		std::uint64_t ownerStableInstanceId = 0u;
		std::uint64_t sourceStableInstanceId = 0u;
		std::uint32_t sourceRecordIndex = kInvalidDeformationRecordIndex;
		std::uint32_t vertexCount = 0u;
		std::uint32_t meshletCount = 0u;
		std::uint32_t clusterCount = 0u;
		std::uint64_t requestedViewMask = 0u;
		bool priorHistoryCompatible = false;
	};

	struct DeformationWorkRecord
	{
		DeformedPageKey key;
		std::uint64_t sourceStableInstanceId = 0u;
		// Ordinary heterogeneous work has exactly one graphics owner. Keep that
		// owner inline so thousands of independent pages do not each allocate a
		// one-element vector; verified clone sharing uses the overflow vector.
		std::uint64_t primaryConsumerStableInstanceId = 0u;
		std::vector<std::uint64_t> additionalConsumerStableInstanceIds;
		std::uint32_t sourceRecordIndex = kInvalidDeformationRecordIndex;
		std::uint32_t vertexOffset = 0u;
		std::uint32_t vertexCount = 0u;
		std::uint32_t meshletBoundsOffset = 0u;
		std::uint32_t meshletCount = 0u;
		std::uint32_t clusterBoundsOffset = 0u;
		std::uint32_t clusterCount = 0u;
		std::uint64_t requestedViewMask = 0u;
		bool historyValid = false;

		[[nodiscard]] std::size_t ConsumerCount() const noexcept
		{
			return (primaryConsumerStableInstanceId != 0u ? 1u : 0u)
				+ additionalConsumerStableInstanceIds.size();
		}
	};

	// Four float4 values give every graphics pass a stable, naturally aligned
	// record. Static UV/color/topology remain in the immutable physical page.
	struct alignas(16) GpuDeformedVertexRecord
	{
		std::array<float, 4> currentPosition{};
		std::array<float, 4> previousPosition{};
		std::array<float, 4> normal{};
		std::array<float, 4> tangentAndSign{};
	};

	struct alignas(16) GpuDynamicBoundsRecord
	{
		std::array<float, 4> minimum{};
		std::array<float, 4> maximum{};
		std::array<float, 4> sphere{};
		std::array<std::uint32_t, 4> identity{};
	};

	// These upload records mirror AnimatedVirtualGeometryAbi.hlsli exactly.
	// Shader backends split full-width generations into two 32-bit lanes without
	// narrowing renderer, scene, residency, or content identities.
	struct alignas(16) GpuDeformationGenerationRecord
	{
		std::uint64_t stableInstanceId = 0u;
		std::uint64_t animationPublicationGeneration = 0u;
		std::uint64_t pageContentGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t sceneGeneration = 0u;
		std::uint64_t meshPayloadGeneration = 0u;
		std::uint64_t reserved = 0u;
	};

	struct alignas(16) GpuDeformationShaderWorkRecord
	{
		std::uint32_t generationRecordIndex = 0u;
		std::uint32_t inputVertexByteOffset = 0u;
		std::uint32_t skinRecordByteOffset = 0u;
		std::uint32_t morphDeltaByteOffset = 0u;
		std::uint32_t vertexCount = 0u;
		std::uint32_t jointCount = 0u;
		std::uint32_t morphTargetCount = 0u;
		std::uint32_t paletteRowOffset = 0u;
		std::uint32_t morphWeightOffset = 0u;
		std::uint32_t currentOutputOffset = 0u;
		std::uint32_t previousOutputOffset = 0u;
		std::uint32_t jointPaletteByteOffset = 0u;
		std::uint32_t flags = 0u;
		std::array<std::uint32_t, 2> reserved{};
		std::uint32_t alignmentPadding = 0u;
		std::array<float, 4> positionOffset{};
		std::array<float, 4> positionScale{};
	};

	struct DeformationConsumerWorkRecord
	{
		std::uint64_t stableInstanceId = 0u;
		std::uint32_t workIndex = 0u;
	};

	struct GpuDeformationPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::vector<DeformationWorkRecord> work;
		// Consumers are ordered once by instance, mesh, and page while the immutable
		// plan is built. Backends use this table to publish grouped draw ownership
		// without reconstructing and sorting the same crowd-sized relation.
		std::vector<DeformationConsumerWorkRecord> orderedConsumers;
		std::uint32_t totalVertexCount = 0u;
		std::uint32_t totalMeshletBoundsCount = 0u;
		std::uint32_t totalClusterBoundsCount = 0u;
		std::uint32_t reusedDemandCount = 0u;
		bool valid = true;
		// Builders validate every demand before publishing the immutable plan. Once
		// sealed, downstream boundaries can reuse that proof instead of rescanning
		// the same records. Manually assembled plans leave this false and use the
		// full validation path in IsComplete().
		bool sealedComplete = false;
		std::string rejectionReason;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	// Accepts page demands as visibility discovers them and folds compatible
	// consumers directly into the immutable work plan. This avoids requiring a
	// second full-size demand array on live crowd frames; callers that already own
	// an array can continue using BuildGpuDeformationPlan below.
	class TERRAENGINE_API GpuDeformationPlanAccumulator
	{
	public:
		GpuDeformationPlanAccumulator(
			std::uint64_t frameId,
			std::uint64_t rendererGeneration,
			std::size_t expectedDemandCount = 0u);
		~GpuDeformationPlanAccumulator();
		GpuDeformationPlanAccumulator(GpuDeformationPlanAccumulator&&) noexcept;
		GpuDeformationPlanAccumulator& operator=(GpuDeformationPlanAccumulator&&) noexcept;
		GpuDeformationPlanAccumulator(const GpuDeformationPlanAccumulator&) = delete;
		GpuDeformationPlanAccumulator& operator=(const GpuDeformationPlanAccumulator&) = delete;

		[[nodiscard]] bool Add(const DeformationPageDemand& demand);
		[[nodiscard]] GpuDeformationPlan Finish() &&;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	TERRAENGINE_API GpuDeformationPlan BuildGpuDeformationPlan(
		std::uint64_t frameId,
		std::uint64_t rendererGeneration,
		std::span<const DeformationPageDemand> demands);

	static_assert(sizeof(GpuDeformedVertexRecord) == 64u,
		"Graphics-facing animated vertices use the frozen v1 64-byte ABI");
	static_assert(sizeof(GpuDynamicBoundsRecord) == 64u,
		"Dynamic bounds use four aligned lanes for portable shader access");
	static_assert(sizeof(GpuDeformationGenerationRecord) == 64u,
		"Deformation generations use eight full-width shader lanes");
	static_assert(sizeof(GpuDeformationShaderWorkRecord) == 96u,
		"Deformation work uploads match the frozen v1 HLSL layout");

}
