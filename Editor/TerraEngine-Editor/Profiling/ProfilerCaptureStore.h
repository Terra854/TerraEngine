#pragma once

#include "Engine/Profiling/ProfilerCapture.h"

#include <filesystem>
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
	bool LoadCapture(const std::filesystem::path& capturePath, TerraEngine::Profiling::ProfilerCaptureDocument& outDocument, std::string& outError) const;
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> ListCaptures(std::string& outError) const;
	bool IsPathInsideProject(const std::filesystem::path& path) const;

private:
	std::filesystem::path MakeCapturePath(const TerraEngine::Profiling::ProfilerCaptureDocument& document) const;

	std::filesystem::path root;
};
