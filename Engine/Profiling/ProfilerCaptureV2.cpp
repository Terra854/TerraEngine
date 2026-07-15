// ProfilerCaptureV2.cpp - Public v2 persistence entry points.
//
// The dispatcher owns format selection in ProfilerCapture.cpp; this module owns
// the v2 API surface and forwards only the fully typed document/artifact values
// into the bounded codec implementation. Keeping this boundary separate lets a
// v1 reader remain frozen while v2 evolves additively.

#include "Profiling/ProfilerCaptureV2.h"

namespace TerraEngine::Profiling
{
	// These internal bodies are intentionally not in the public header. They
	// share the dispatcher translation unit's legacy conversion helpers while
	// this file remains the dedicated v2 implementation/API boundary.
	bool SaveProfilerCaptureV2Impl(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError);
	bool LoadProfilerCaptureV2Impl(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError);
	bool TryReadProfilerCaptureV2MetadataImpl(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact);

	bool SaveProfilerCaptureV2(const ProfilerCaptureDocument& document, const std::filesystem::path& path, std::string& outError)
	{
		return SaveProfilerCaptureV2Impl(document, path, outError);
	}

	bool LoadProfilerCaptureV2(const std::filesystem::path& path, ProfilerCaptureDocument& outDocument, std::string& outError)
	{
		return LoadProfilerCaptureV2Impl(path, outDocument, outError);
	}

	bool TryReadProfilerCaptureV2Metadata(const std::filesystem::path& path, ProfilerCaptureArtifact& outArtifact)
	{
		return TryReadProfilerCaptureV2MetadataImpl(path, outArtifact);
	}
}
