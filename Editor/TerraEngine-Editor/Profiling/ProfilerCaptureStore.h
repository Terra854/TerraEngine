// ProfilerCaptureStore.h — Profiler capture storage management
//
// Manages the discovery and persistence of profiler capture files within a project. it provides helpers for generating deterministic capture paths, validating project-relative locations, and listing available artifacts for the editor UI.

#pragma once

#include "Engine/Profiling/ProfilerCapture.h"
#include "Engine/Profiling/ProfilerJournalV1.h"

#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <vector>

// ProfilerCaptureIndexPage is the editor-facing ownership boundary for a V1
// journal index. The engine reader streams checkpoint rows into only the
// requested page, so neither workers nor UI snapshots materialize the history.
struct ProfilerCaptureIndexPage
{
	TerraEngine::Profiling::ProfilerCaptureMetadata metadata;
	TerraEngine::Profiling::ProfilerJournalV1Summary summary;
	std::size_t offset = 0u;
	std::size_t pageSize = 0u;
	std::size_t totalFrameCount = 0u;
	std::vector<TerraEngine::Profiling::ProfilerFrameListItem> frames;
	bool complete = false;
	bool recoveredIncompleteTail = false;
};

class ProfilerCaptureStore
{
public:
	// A request for this offset opens the newest complete page without asking a
	// caller to first retain the total history length.
	static constexpr std::size_t kNewestIndexPageOffset =
		std::numeric_limits<std::size_t>::max();
	static constexpr std::size_t kMaximumIndexPageSize = 512u;

	explicit ProfilerCaptureStore(std::filesystem::path projectRoot = {});

	void SetProjectRoot(const std::filesystem::path& projectRoot);
	void ClearProjectRoot();
	bool HasProjectRoot() const;
	std::filesystem::path GetProfilerDirectory() const;
	// Reserve one unique project-contained V1 path. The engine writer validates
	// and commits its header before producers become active.
	bool ReserveJournalArtifact(
		const TerraEngine::Profiling::ProfilerCaptureMetadata& metadata,
		std::filesystem::path& outPath,
		std::string& outError) const;
	bool ReadJournalIndex(
		const std::filesystem::path& capturePath,
		TerraEngine::Profiling::ProfilerJournalV1OpenResult& outResult,
		std::string& outError) const;
	// Reads metadata and one compact frame-list page. Detailed frame payloads
	// stay on disk until ReadSelectedJournalFrame is explicitly requested.
	bool ReadJournalIndexPage(
		const std::filesystem::path& capturePath,
		std::size_t offset,
		std::size_t pageSize,
		ProfilerCaptureIndexPage& outPage,
		std::string& outError) const;
	bool ReadSelectedJournalFrame(
		const std::filesystem::path& capturePath,
		std::uint64_t frameIndex,
		TerraEngine::Profiling::ProfilerFrameRecord& outFrame,
		std::string& outError) const;

	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> ListCaptures(std::string& outError, const std::function<bool()>& shouldCancel = {}) const;
	bool IsPathInsideProject(const std::filesystem::path& path) const;

private:
	std::filesystem::path MakeJournalPath(const TerraEngine::Profiling::ProfilerCaptureMetadata& metadata) const;

	std::filesystem::path root;
};
