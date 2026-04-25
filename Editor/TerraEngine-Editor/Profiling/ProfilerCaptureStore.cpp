#include "ProfilerCaptureStore.h"

#include <algorithm>
#include <system_error>

namespace
{
std::filesystem::path WeaklyCanonicalOrAbsolute(const std::filesystem::path& path)
{
	std::error_code ec;
	std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
	if (!ec)
	{
		return canonical;
	}
	return std::filesystem::absolute(path);
}

bool PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& parent)
{
	const std::filesystem::path normalizedPath = WeaklyCanonicalOrAbsolute(path);
	const std::filesystem::path normalizedParent = WeaklyCanonicalOrAbsolute(parent);
	auto pathIt = normalizedPath.begin();
	auto parentIt = normalizedParent.begin();
	for (; parentIt != normalizedParent.end(); ++parentIt, ++pathIt)
	{
		if (pathIt == normalizedPath.end() || *pathIt != *parentIt)
		{
			return false;
		}
	}
	return true;
}
}

ProfilerCaptureStore::ProfilerCaptureStore(std::filesystem::path projectRoot)
{
	SetProjectRoot(projectRoot);
}

void ProfilerCaptureStore::SetProjectRoot(const std::filesystem::path& projectRoot)
{
	if (projectRoot.empty())
	{
		root.clear();
		return;
	}
	root = WeaklyCanonicalOrAbsolute(projectRoot);
}

void ProfilerCaptureStore::ClearProjectRoot()
{
	root.clear();
}

bool ProfilerCaptureStore::HasProjectRoot() const
{
	return !root.empty();
}

std::filesystem::path ProfilerCaptureStore::GetProfilerDirectory() const
{
	if (root.empty())
	{
		return {};
	}
	return root / "Profiler";
}

bool ProfilerCaptureStore::SaveNewCapture(TerraEngine::Profiling::ProfilerCaptureDocument document, std::filesystem::path& outPath, std::string& outError) const
{
	if (root.empty())
	{
		outError = "No project is open.";
		return false;
	}

	const std::filesystem::path profilerDirectory = GetProfilerDirectory();
	if (!IsPathInsideProject(profilerDirectory))
	{
		outError = "Profiler directory is outside the opened project.";
		return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(profilerDirectory, ec);
	if (ec)
	{
		outError = "Failed to create project Profiler directory: " + ec.message();
		return false;
	}

	std::filesystem::path capturePath = MakeCapturePath(document);
	if (!IsPathInsideProject(capturePath))
	{
		outError = "Profiler capture path is outside the opened project.";
		return false;
	}

	document.sourcePath = capturePath;
	document.metadata.projectPathHint = root.string();
	document.metadata.savedAt = TerraEngine::Profiling::MakeReadableTimestamp();
	if (!TerraEngine::Profiling::SaveCaptureDocument(document, capturePath, outError))
	{
		return false;
	}

	outPath = capturePath;
	return true;
}

bool ProfilerCaptureStore::LoadCapture(const std::filesystem::path& capturePath, TerraEngine::Profiling::ProfilerCaptureDocument& outDocument, std::string& outError) const
{
	if (root.empty())
	{
		outError = "No project is open.";
		return false;
	}
	if (!IsPathInsideProject(capturePath) || !PathStartsWith(capturePath, GetProfilerDirectory()))
	{
		outError = "Profiler captures can only be loaded from the opened project's Profiler folder.";
		return false;
	}

	if (!TerraEngine::Profiling::LoadCaptureDocument(capturePath, outDocument, outError))
	{
		return false;
	}
	outDocument.sourcePath = WeaklyCanonicalOrAbsolute(capturePath);
	return true;
}

std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> ProfilerCaptureStore::ListCaptures(std::string& outError) const
{
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
	outError.clear();

	if (root.empty())
	{
		return captures;
	}

	const std::filesystem::path profilerDirectory = GetProfilerDirectory();
	if (!IsPathInsideProject(profilerDirectory))
	{
		outError = "Profiler directory is outside the opened project.";
		return captures;
	}

	std::error_code ec;
	if (!std::filesystem::exists(profilerDirectory, ec))
	{
		return captures;
	}

	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(profilerDirectory, ec))
	{
		if (ec)
		{
			outError = ec.message();
			break;
		}
		if (!entry.is_regular_file() || entry.path().extension() != ".teprof")
		{
			continue;
		}

		TerraEngine::Profiling::ProfilerCaptureArtifact artifact;
		TerraEngine::Profiling::TryReadCaptureArtifact(entry.path(), artifact);
		if (artifact.supported)
		{
			captures.push_back(std::move(artifact));
		}
	}

	std::sort(captures.begin(), captures.end(), [](const auto& left, const auto& right) {
		return left.fileName > right.fileName;
	});
	return captures;
}

bool ProfilerCaptureStore::IsPathInsideProject(const std::filesystem::path& path) const
{
	if (root.empty() || path.empty())
	{
		return false;
	}
	return PathStartsWith(path, root);
}

std::filesystem::path ProfilerCaptureStore::MakeCapturePath(const TerraEngine::Profiling::ProfilerCaptureDocument& document) const
{
	std::string projectName = document.metadata.projectName.empty() ? "Capture" : document.metadata.projectName;
	for (char& character : projectName)
	{
		if (character == '/' || character == '\\' || character == ':' || character == '*' || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
		{
			character = '_';
		}
	}
	return GetProfilerDirectory() / (projectName + "-" + TerraEngine::Profiling::MakeTimestampForFileName() + ".teprof");
}
