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

		[[nodiscard]] bool IsComplete() const noexcept;
		auto operator<=>(const StaticPageKey&) const = default;
	};

	struct DeformedPageKey
	{
		StaticPageKey page;
		DeformationGeneration deformation;
		std::uint32_t inputLayoutVersion = 0u;

		[[nodiscard]] bool IsComplete() const noexcept;
		auto operator<=>(const DeformedPageKey&) const = default;
	};

	struct DeformationPageDemand
	{
		DeformedPageKey key;
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
		std::uint32_t sourceRecordIndex = kInvalidDeformationRecordIndex;
		std::uint32_t vertexOffset = 0u;
		std::uint32_t vertexCount = 0u;
		std::uint32_t meshletBoundsOffset = 0u;
		std::uint32_t meshletCount = 0u;
		std::uint32_t clusterBoundsOffset = 0u;
		std::uint32_t clusterCount = 0u;
		std::uint64_t requestedViewMask = 0u;
		bool historyValid = false;
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
	// Full-width generations are split by SPIR-V into uint2 lanes without
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

	struct GpuDeformationPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::vector<DeformationWorkRecord> work;
		std::uint32_t totalVertexCount = 0u;
		std::uint32_t totalMeshletBoundsCount = 0u;
		std::uint32_t totalClusterBoundsCount = 0u;
		std::uint32_t reusedDemandCount = 0u;
		bool valid = true;
		std::string rejectionReason;

		[[nodiscard]] bool IsComplete() const noexcept;
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
