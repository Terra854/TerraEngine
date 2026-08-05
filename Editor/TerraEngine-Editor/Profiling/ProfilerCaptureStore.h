// ProfilerCaptureStore.h — Profiler capture storage management
//
// Manages the discovery and persistence of profiler capture files within a project. it provides helpers for generating deterministic capture paths, validating project-relative locations, and listing available artifacts for the editor UI.

#pragma once

#include "Engine/Profiling/ProfilerCapture.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class ProfilerCaptureStore
{
public:
	explicit ProfilerCaptureStore(std::filesystem::path projectRoot = {});

	void SetProjectRoot(const std::filesystem::path& projectRoot);
	void ClearProjectRoot();
	bool HasProjectRoot() const;
	std::filesystem::path GetProfilerDirectory() const;

	bool SaveNewCapture(TerraEngine::Profiling::ProfilerCaptureDocument document, std::filesystem::path& outPath, std::string& outError) const;
	// The editor's asynchronous finish path already owns its document. Retaining
	// that value after serialization avoids reading a multi-megabyte capture back
	// from disk merely to select the session that was just recorded.
	bool SaveNewCaptureRetained(TerraEngine::Profiling::ProfilerCaptureDocument& document, std::filesystem::path& outPath, std::string& outError) const;
	bool LoadCapture(const std::filesystem::path& capturePath, TerraEngine::Profiling::ProfilerCaptureDocument& outDocument, std::string& outError) const;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> ListCaptures(std::string& outError, const std::function<bool()>& shouldCancel = {}) const;
	bool IsPathInsideProject(const std::filesystem::path& path) const;

private:
	std::filesystem::path MakeCapturePath(const TerraEngine::Profiling::ProfilerCaptureDocument& document) const;

	std::filesystem::path root;
};
