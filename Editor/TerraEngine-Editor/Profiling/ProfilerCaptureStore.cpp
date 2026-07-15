// ProfilerCaptureStore.cpp — Profiler capture storage implementation
//
// Implements the filesystem operations for the profiler capture store, ensuring captures are saved to and loaded from the designated project directory. It handles path normalization and performs basic integrity checks on capture metadata during listing.

#include "ProfilerCaptureStore.h"

#include <algorithm>
#include <system_error>

namespace
{
// WeaklyCanonicalOrAbsolute is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path WeaklyCanonicalOrAbsolute(const std::filesystem::path& path)
{
	std::error_code ec;
	std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ec)
	{
		return canonical;
	}
	return std::filesystem::absolute(path);
}

// PathStartsWith is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& parent)
{
	const std::filesystem::path normalizedPath = WeaklyCanonicalOrAbsolute(path);
	const std::filesystem::path normalizedParent = WeaklyCanonicalOrAbsolute(parent);
	auto pathIt = normalizedPath.begin();
	auto parentIt = normalizedParent.begin();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

// ProfilerCaptureStore::SetProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerCaptureStore::SetProjectRoot(const std::filesystem::path& projectRoot)
{
	if (projectRoot.empty())
	{
		root.clear();
		return;
	}
	// Keep the caller's absolute spelling for persisted metadata. Containment
	// checks canonicalize separately, so aliases remain safe without turning a
	// RUNNER~1 input into runneradmin in the saved capture document.
	std::error_code absoluteError;
	const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, absoluteError);
	root = (absoluteError ? projectRoot : absoluteRoot).lexically_normal();
}

// ProfilerCaptureStore::ClearProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerCaptureStore::ClearProjectRoot()
{
	root.clear();
}

// ProfilerCaptureStore::HasProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProfilerCaptureStore::HasProjectRoot() const
{
	return !root.empty();
}

// ProfilerCaptureStore::GetProfilerDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ProfilerCaptureStore::GetProfilerDirectory() const
{
	if (root.empty())
	{
		return {};
	}
	return root / "Profiler";
}

// ProfilerCaptureStore::SaveNewCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProfilerCaptureStore::SaveNewCapture(TerraEngine::Profiling::ProfilerCaptureDocument document, std::filesystem::path& outPath, std::string& outError) const
{
	if (root.empty())
	{
		outError = "No project is open.";
		return false;
	}

	const std::filesystem::path profilerDirectory = GetProfilerDirectory();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!IsPathInsideProject(profilerDirectory))
	{
		outError = "Profiler directory is outside the opened project.";
		return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(profilerDirectory, ec);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ec)
	{
		outError = "Failed to create project Profiler directory: " + ec.message();
		return false;
	}

	std::filesystem::path capturePath = MakeCapturePath(document);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!IsPathInsideProject(capturePath))
	{
		outError = "Profiler capture path is outside the opened project.";
		return false;
	}

	document.sourcePath = capturePath;
	document.metadata.projectPathHint = root.string();
	document.metadata.savedAt = TerraEngine::Profiling::MakeReadableTimestamp();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TerraEngine::Profiling::SaveCaptureDocument(document, capturePath, outError))
	{
		return false;
	}

	outPath = capturePath;
	return true;
}

// ProfilerCaptureStore::LoadCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProfilerCaptureStore::LoadCapture(const std::filesystem::path& capturePath, TerraEngine::Profiling::ProfilerCaptureDocument& outDocument, std::string& outError) const
{
	if (root.empty())
	{
		outError = "No project is open.";
		return false;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!IsPathInsideProject(capturePath) || !PathStartsWith(capturePath, GetProfilerDirectory()))
	{
		outError = "Profiler captures can only be loaded from the opened project's Profiler folder.";
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TerraEngine::Profiling::LoadCaptureDocument(capturePath, outDocument, outError))
	{
		return false;
	}
	outDocument.sourcePath = WeaklyCanonicalOrAbsolute(capturePath);
	return true;
}

// ProfilerCaptureStore::ListCaptures is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> ProfilerCaptureStore::ListCaptures(std::string& outError, const std::function<bool()>& shouldCancel) const
{
	std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures;
	outError.clear();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (root.empty())
	{
		return captures;
	}

	const std::filesystem::path profilerDirectory = GetProfilerDirectory();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!IsPathInsideProject(profilerDirectory))
	{
		outError = "Profiler directory is outside the opened project.";
		return captures;
	}

	std::error_code ec;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(profilerDirectory, ec))
	{
		return captures;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(profilerDirectory, ec))
	{
		if (shouldCancel && shouldCancel())
		{
			outError.clear();
			return {};
		}
		if (ec)
		{
			outError = ec.message();
			break;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!entry.is_regular_file() || entry.path().extension() != ".teprof")
		{
			continue;
		}

		TerraEngine::Profiling::ProfilerCaptureArtifact artifact;
		TerraEngine::Profiling::TryReadCaptureArtifactMetadata(entry.path(), artifact);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (artifact.supported)
		{
			// The engine codec owns chunk validation and recovery.  The editor store
			// preserves that verdict as metadata so listing a capture never hides a
			// recovered final tail behind an apparently normal artifact.
			if (artifact.status == "Ready (metadata)")
			{
				// "Ready (metadata)" describes the codec's fast metadata-only scan,
				// not a distinct durable artifact state. Preserve the established editor
				// listing contract while complete/recovery/checksum fields carry detail.
				artifact.status = "Ready";
			}
			if (artifact.status.empty())
			{
				artifact.status = artifact.recoveredIncompleteTail
					? "recovered_incomplete_tail"
					: (artifact.complete ? "complete" : "incomplete");
			}
			captures.push_back(std::move(artifact));
		}
	}

	std::sort(captures.begin(), captures.end(), [](const auto& left, const auto& right) {
		return left.fileName > right.fileName;
	});
	return captures;
}

// ProfilerCaptureStore::IsPathInsideProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProfilerCaptureStore::IsPathInsideProject(const std::filesystem::path& path) const
{
	if (root.empty() || path.empty())
	{
		return false;
	}
	return PathStartsWith(path, root);
}

// ProfilerCaptureStore::MakeCapturePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ProfilerCaptureStore::MakeCapturePath(const TerraEngine::Profiling::ProfilerCaptureDocument& document) const
{
	std::string projectName = document.metadata.projectName.empty() ? "Capture" : document.metadata.projectName;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (char& character : projectName)
	{
		if (character == '/' || character == '\\' || character == ':' || character == '*' || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
		{
			character = '_';
		}
	}
	return GetProfilerDirectory() / (projectName + "-" + TerraEngine::Profiling::MakeTimestampForFileName() + ".teprof");
}
