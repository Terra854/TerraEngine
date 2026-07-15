// ProfilerCaptureV2.h - Compact, recoverable profiler-capture v2 codec.
//
// The public v2 helpers deliberately operate on the ordinary capture document.
// Editor callers therefore need no knowledge of chunk layout, string ids, or
// checksum details and can load v1/v2 artifacts through one dispatcher.

#pragma once

#include "Profiling/ProfilerCapture.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace TerraEngine::Profiling
{
	inline constexpr std::uint32_t kProfilerCaptureV2PayloadVersion = 2u;

	// SaveProfilerCaptureV2 writes a new artifact through a sibling temporary
	// file and atomically replaces the destination only after a complete index
	// has been written. Existing v1 fixtures are never modified by this path.
	[[nodiscard]] bool SaveProfilerCaptureV2(
		const ProfilerCaptureDocument& document,
		const std::filesystem::path& path,
		std::string& outError);

	// LoadProfilerCaptureV2 accepts every valid committed chunk. If a final
	// chunk/index is incomplete, it returns the preceding committed frames and
	// marks recoveredIncompleteTail instead of discarding useful evidence.
	[[nodiscard]] bool LoadProfilerCaptureV2(
		const std::filesystem::path& path,
		ProfilerCaptureDocument& outDocument,
		std::string& outError);

	// TryReadProfilerCaptureV2Metadata reads recovery/index facts without
	// materializing every frame. It is intended for capture-store listing.
	[[nodiscard]] bool TryReadProfilerCaptureV2Metadata(
		const std::filesystem::path& path,
		ProfilerCaptureArtifact& outArtifact);
}
