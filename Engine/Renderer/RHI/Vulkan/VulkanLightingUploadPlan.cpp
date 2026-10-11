#include "Renderer/RHI/Vulkan/VulkanLightingUploadPlan.h"
#include "Renderer/RenderTypes.h"
#include <algorithm>
#include <bit>
#include <limits>

namespace TerraEngine::Renderer::VulkanLightingUpload
{
	void CanonicalizeLighting(std::span<const GPULight> lights, CanonicalBlock& output)
	{
		// Serialize fields rather than the C++ object representation: all 44
		// lanes are semantic (including the LTC selectors), but padding is not.
		// Zero the inactive tail in place, avoiding a 180 KiB stack temporary.
		output.records.fill(CanonicalRecord{});
		output.activeCount = static_cast<std::uint32_t>(std::min<std::size_t>(lights.size(), kRecordCount));
		for (std::uint32_t index = 0; index < output.activeCount; ++index)
		{
			const auto& light = lights[index];
			const std::array<const glm::vec4*, 11> fields{ &light.positionAndType, &light.directionAndRange,
				&light.colorAndIntensity, &light.coneAnglesAndArea, &light.shadowAndPadding, &light.shadowRegion,
				&light.extraParams, &light.shadowMatrixC0, &light.shadowMatrixC1, &light.shadowMatrixC2, &light.shadowMatrixC3 };
			for (std::size_t field = 0; field < fields.size(); ++field)
				for (std::size_t lane = 0; lane < 4; ++lane)
					output.records[index][field * 4 + lane] = std::bit_cast<std::uint32_t>((*fields[field])[static_cast<int>(lane)]);
		}
	}
	bool BuildLightingDirtyRangePlan(const PhysicalOwner& owner, const MappingBounds& bounds,
		const CanonicalBlock& desired, const Mirror& mirror, DirtyRangePlan& output)
	{
		// A failed plan cannot leave a previous valid plan available to a caller.
		// The large owned byte block is copied only after input validation.
		output.valid = false;
		output.logicalRuns.clear();
		output.flushRanges.clear();
		output.logicalBytes = 0;
		output.physicalFlushBytes = 0;
		output.fullInitialization = false;
		const auto maximum = std::numeric_limits<std::uint64_t>::max();
		auto checkedEnd = [maximum](std::uint64_t offset, std::uint64_t size, std::uint64_t& end) {
			if (size > maximum - offset) return false;
			end = offset + size;
			return true;
		};
		std::uint64_t regionEnd = 0, mappedEnd = 0;
		if (owner.allocationId == 0 || owner.allocationGeneration == 0 || owner.rendererSessionGeneration == 0
			|| owner.frameSlot >= 3 || owner.snapshotSlot >= 8 || owner.regionSizeBytes != kBlockBytes
			|| static_cast<unsigned>(owner.purpose) > static_cast<unsigned>(Purpose::AnalysisInput)
			|| static_cast<unsigned>(owner.viewType) > static_cast<unsigned>(RenderViewType::ExportRuntime)
			|| desired.activeCount > kRecordCount || mirror.revision == maximum
			|| !checkedEnd(owner.regionOffsetBytes, kBlockBytes, regionEnd)
			|| !checkedEnd(bounds.mappedOffsetBytes, bounds.mappedSizeBytes, mappedEnd)
			|| regionEnd > bounds.allocationSizeBytes || mappedEnd > bounds.allocationSizeBytes
			|| owner.regionOffsetBytes < bounds.mappedOffsetBytes || regionEnd > mappedEnd
			|| (!bounds.coherent && bounds.nonCoherentAtomSize == 0)) return false;
		// An externally constructed block must obey the same zero-tail contract
		// as CanonicalizeLighting, otherwise shrink could retain stale contents.
		for (std::uint32_t index = desired.activeCount; index < kRecordCount; ++index)
			for (auto word : desired.records[index]) if (word != 0) return false;
		output.owner = owner;
		output.desiredBlock = desired;
		output.expectedMirrorRevision = mirror.revision;
		output.coherent = bounds.coherent;
		output.fullInitialization = !mirror.initialized || mirror.owner != owner;
		if (output.fullInitialization) output.logicalRuns.push_back({ 0, kRecordCount });
		else
		{
			// Scan in record order. Only truly adjacent dirty records share a run;
			// a clean record between edits never becomes an invented host write.
			for (std::uint32_t index = 0; index < kRecordCount;)
			{
				if (desired.records[index] == mirror.block.records[index]) { ++index; continue; }
				const auto first = index++;
				while (index < kRecordCount && desired.records[index] != mirror.block.records[index]) ++index;
				output.logicalRuns.push_back({ first, index - first });
			}
		}
		for (const auto& run : output.logicalRuns)
		{
			const auto size = static_cast<std::uint64_t>(run.recordCount) * kRecordBytes;
			output.logicalBytes += size;
			if (bounds.coherent) continue;
			const auto start = owner.regionOffsetBytes + static_cast<std::uint64_t>(run.firstRecord) * kRecordBytes;
			const auto end = start + size;
			const auto atom = bounds.nonCoherentAtomSize;
			const auto alignedStart = start - start % atom;
			const auto remainder = end % atom;
			const auto extra = remainder == 0 ? 0 : atom - remainder;
			// Vulkan permits a final atom ending at the allocation's actual end.
			// Never extend a flush outside that allocation or its mapped interval.
			const auto alignedEnd = extra > bounds.allocationSizeBytes - end ? bounds.allocationSizeBytes : end + extra;
			if (alignedStart < bounds.mappedOffsetBytes || alignedEnd > mappedEnd)
			{
				output.logicalRuns.clear();
				output.flushRanges.clear();
				output.logicalBytes = output.physicalFlushBytes = 0;
				return false;
			}
			if (!output.flushRanges.empty())
			{
				auto& previous = output.flushRanges.back();
				if (alignedStart <= previous.offsetBytes + previous.sizeBytes)
				{
					previous.sizeBytes = std::max(previous.offsetBytes + previous.sizeBytes, alignedEnd) - previous.offsetBytes;
					continue;
				}
			}
			output.flushRanges.push_back({ alignedStart, alignedEnd - alignedStart });
		}
		for (const auto& range : output.flushRanges) output.physicalFlushBytes += range.sizeBytes;
		output.valid = true;
		return true;
	}
	bool CommitLightingUpload(const DirtyRangePlan& plan, const UploadReceipt& receipt, Mirror& mirror)
	{
		// Check revision before handling failure: an old async result must never
		// revoke a newer upload's safety proof or replace its successful contents.
		if (!plan.valid || mirror.revision != plan.expectedMirrorRevision
			|| receipt.expectedMirrorRevision != plan.expectedMirrorRevision || receipt.owner != plan.owner
			|| mirror.revision == std::numeric_limits<std::uint64_t>::max()) return false;
		const bool writesNeeded = plan.logicalBytes != 0;
		const bool succeeded = receipt.frameSlotSafe && receipt.ownerCurrent
			&& (!writesNeeded || (receipt.mapSucceeded && receipt.writeSucceeded && (plan.coherent || receipt.flushSucceeded)));
		if (!succeeded)
		{
			// Partial host writes are possible even when flush fails. Keep the last
			// successful diagnostic bytes, but force full initialization on retry.
			InvalidateLightingMirror(mirror);
			return false;
		}
		mirror.owner = plan.owner;
		mirror.block = plan.desiredBlock;
		mirror.initialized = true;
		++mirror.revision;
		return true;
	}
	void InvalidateLightingMirror(Mirror& mirror)
	{
		mirror.initialized = false;
		// Exhaustion remains permanently invalid instead of wrapping and making
		// a very old plan's revision look current again.
		if (mirror.revision != std::numeric_limits<std::uint64_t>::max()) ++mirror.revision;
	}
}
