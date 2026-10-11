#pragma once

#include "Core/Export.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

struct GPULight;
enum class RenderViewType : std::uint8_t;

namespace TerraEngine::Renderer::VulkanLightingUpload
{
	inline constexpr std::uint32_t kRecordCount = 1024;
	inline constexpr std::uint32_t kWordsPerRecord = 44;
	inline constexpr std::uint64_t kRecordBytes = 176;
	inline constexpr std::uint64_t kBlockBytes = kRecordCount * kRecordBytes;
	using CanonicalRecord = std::array<std::uint32_t, kWordsPerRecord>;
	struct CanonicalBlock
	{
		std::array<CanonicalRecord, kRecordCount> records{};
		std::uint32_t activeCount = 0;
		bool operator==(const CanonicalBlock&) const = default;
	};
	enum class Purpose : std::uint8_t { FinalLight, AnalysisInput };
	// This names a physical region, not a frame serial. A view reassignment still
	// requires initialization even when its bytes happen to match the old view.
	struct PhysicalOwner
	{
		std::uint64_t allocationId = 0, allocationGeneration = 0, rendererSessionGeneration = 0;
		std::uint32_t frameSlot = 0, snapshotSlot = 0;
		Purpose purpose = Purpose::FinalLight;
		RenderViewType viewType{};
		std::uint32_t outputId = 0;
		std::uint64_t regionOffsetBytes = 0, regionSizeBytes = kBlockBytes;
		bool operator==(const PhysicalOwner&) const = default;
	};
	struct MappingBounds
	{
		std::uint64_t allocationSizeBytes = 0, mappedOffsetBytes = 0, mappedSizeBytes = 0;
		std::uint64_t nonCoherentAtomSize = 1;
		bool coherent = false;
	};
	struct RecordRun
	{
		std::uint32_t firstRecord = 0, recordCount = 0;
		bool operator==(const RecordRun&) const = default;
	};
	struct FlushRange
	{
		std::uint64_t offsetBytes = 0, sizeBytes = 0;
		bool operator==(const FlushRange&) const = default;
	};
	struct Mirror
	{
		PhysicalOwner owner{};
		CanonicalBlock block{};
		std::uint64_t revision = 0;
		bool initialized = false;
	};
	// The plan owns its candidate bytes, so a worker cannot change them between
	// planning and the protected upload/commit boundary.
	struct DirtyRangePlan
	{
		PhysicalOwner owner{};
		CanonicalBlock desiredBlock{};
		std::uint64_t expectedMirrorRevision = 0;
		std::vector<RecordRun> logicalRuns;
		std::vector<FlushRange> flushRanges;
		std::uint64_t logicalBytes = 0, physicalFlushBytes = 0;
		bool coherent = false, fullInitialization = false, valid = false;
	};
	struct UploadReceipt
	{
		PhysicalOwner owner{};
		std::uint64_t expectedMirrorRevision = 0;
		bool frameSlotSafe = false, ownerCurrent = false;
		bool mapSucceeded = false, writeSucceeded = false, flushSucceeded = false;
	};
	TERRAENGINE_API void CanonicalizeLighting(std::span<const GPULight> lights, CanonicalBlock& output);
	TERRAENGINE_API bool BuildLightingDirtyRangePlan(const PhysicalOwner& owner, const MappingBounds& bounds,
		const CanonicalBlock& desired, const Mirror& mirror, DirtyRangePlan& output);
	TERRAENGINE_API bool CommitLightingUpload(const DirtyRangePlan& plan, const UploadReceipt& receipt, Mirror& mirror);
	TERRAENGINE_API void InvalidateLightingMirror(Mirror& mirror);
}
