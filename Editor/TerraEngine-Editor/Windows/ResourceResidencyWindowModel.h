#pragma once

#include "Engine/Resource/ResourceStreaming.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace TerraEngine::Editor
{
	struct ResourceResidencyByteDisplay
	{
		std::string label;
		std::string tooltip;
		std::uint64_t sortKey = 0u;
		bool numeric = true;
	};

	inline ResourceResidencyByteDisplay MakeResourceResidencyByteDisplay(
		const Resource::ExactResidencyByteValue& value,
		std::string_view unavailableDomain)
	{
		ResourceResidencyByteDisplay display;
		if (value.status == Resource::ExactResidencyByteStatus::Exact
			|| value.status == Resource::ExactResidencyByteStatus::ZeroByState)
		{
			display.label = std::to_string(value.bytes);
			display.sortKey = value.bytes;
			display.numeric = true;
			if (value.status == Resource::ExactResidencyByteStatus::ZeroByState)
			{
				display.tooltip = "Exact zero: current asset state owns no live bytes in this memory domain.";
			}
			return display;
		}

		display.label = "unavailable";
		display.sortKey = std::numeric_limits<std::uint64_t>::max();
		display.numeric = false;
		display.tooltip = value.unavailableReason.empty()
			? std::string(unavailableDomain) + " exact byte producer did not publish a value."
			: value.unavailableReason;
		return display;
	}

	inline bool HasExactLiveResidencyBytes(const Resource::StreamingDiagnosticRecord& record)
	{
		const bool hasCpuBytes = record.exactCpuBytes.status == Resource::ExactResidencyByteStatus::Exact
			&& record.exactCpuBytes.bytes > 0u;
		const bool hasGpuBytes = record.exactGpuBytes.status == Resource::ExactResidencyByteStatus::Exact
			&& record.exactGpuBytes.bytes > 0u;
		return hasCpuBytes || hasGpuBytes;
	}

	inline bool ShouldDisplayStreamingResidencyRecord(
		const Resource::StreamingDiagnosticRecord& record,
		bool isGeneratedPackageParent)
	{
		// Generated GLB/FBX package parents are logical containers. Show them
		// only if they own proven live bytes themselves; child mesh/material/
		// texture rows are the source of truth for imported payload memory.
		return !isGeneratedPackageParent || HasExactLiveResidencyBytes(record);
	}

	inline std::string ExactStreamingMemoryLocation(const Resource::StreamingDiagnosticRecord& record)
	{
		const bool hasCpuBytes = record.exactCpuBytes.status == Resource::ExactResidencyByteStatus::Exact
			&& record.exactCpuBytes.bytes > 0u;
		const bool hasGpuBytes = record.exactGpuBytes.status == Resource::ExactResidencyByteStatus::Exact
			&& record.exactGpuBytes.bytes > 0u;
		if (record.payloadState == Resource::StreamablePayloadState::PendingRelease)
		{
			return "pending unload";
		}
		if (record.payloadState == Resource::StreamablePayloadState::Failed)
		{
			return "failed";
		}
		if (hasCpuBytes && hasGpuBytes)
		{
			return "RAM + VRAM";
		}
		if (hasCpuBytes)
		{
			return "RAM";
		}
		if (hasGpuBytes)
		{
			return "VRAM";
		}
		return "not loaded";
	}
}
