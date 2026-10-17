// ProfilerCaptureStore.cpp — Profiler capture storage implementation
//
// Implements the filesystem operations for the profiler capture store, ensuring captures are saved to and loaded from the designated project directory. It handles path normalization and performs basic integrity checks on capture metadata during listing.

#include "ProfilerCaptureStore.h"

#include "Engine/Profiling/ProfilerJournalV1.h"

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

// MakeJournalArtifact converts the bounded V1 header/index result into the
// editor's temporary catalog bridge. It intentionally does not request a full
// document, so listing a long capture remains independent of sample volume.
TerraEngine::Profiling::ProfilerCaptureArtifact MakeJournalArtifact(
	const std::filesystem::path& path,
	const TerraEngine::Profiling::ProfilerJournalV1OpenResult& journal)
{
	TerraEngine::Profiling::ProfilerCaptureArtifact artifact;
	artifact.fileName = path.filename().string();
	artifact.absolutePath = WeaklyCanonicalOrAbsolute(path);
	artifact.metadata = journal.metadata;
	artifact.frameCount = journal.summary.frameCount;
	artifact.committedFrameCount = journal.summary.frameCount;
	artifact.complete = journal.complete;
	artifact.recoveredIncompleteTail = journal.recoveredIncompleteTail;
	artifact.supported = true;
	artifact.status = journal.recoveredIncompleteTail
		? "Recovered incomplete tail"
		: (journal.complete ? "Ready" : "Incomplete");
	return artifact;
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
	// Keep the caller's absolute spelling for persisted metadata. Containment
	// checks canonicalize separately, so aliases remain safe without turning a
	// RUNNER~1 input into runneradmin in the saved capture document.
	std::error_code absoluteError;
	const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, absoluteError);
	root = (absoluteError ? projectRoot : absoluteRoot).lexically_normal();
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

bool ProfilerCaptureStore::ReserveJournalArtifact(
	const TerraEngine::Profiling::ProfilerCaptureMetadata& metadata,
	std::filesystem::path& outPath,
	std::string& outError) const
{
	outPath.clear();
	outError.clear();
	if (root.empty())
	{
		outError = "No project is open.";
		return false;
	}
	const std::filesystem::path directory = GetProfilerDirectory();
	if (!IsPathInsideProject(directory))
	{
		outError = "Profiler directory is outside the opened project.";
		return false;
	}
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	if (error)
	{
		outError = "Failed to create project Profiler directory: " + error.message();
		return false;
	}
	std::filesystem::path candidate = MakeJournalPath(metadata);
	for (std::uint32_t suffix = 1u; std::filesystem::exists(candidate, error); ++suffix)
	{
		if (error || suffix == 10000u)
		{
			outError = error ? error.message() : "Could not reserve a unique profiler journal filename.";
			return false;
		}
		candidate = directory / (candidate.stem().string() + "-" + std::to_string(suffix) + candidate.extension().string());
	}
	if (!IsPathInsideProject(candidate))
	{
		outError = "Profiler journal path is outside the opened project.";
		return false;
	}
	outPath = candidate;
	return true;
}

bool ProfilerCaptureStore::ReadJournalIndex(
	const std::filesystem::path& capturePath,
	TerraEngine::Profiling::ProfilerJournalV1OpenResult& outResult,
	std::string& outError) const
{
	if (root.empty() || !IsPathInsideProject(capturePath) || !PathStartsWith(capturePath, GetProfilerDirectory()))
	{
		outError = "Profiler captures can only be read from the opened project's Profiler folder.";
		return false;
	}
	// Finalization and catalog callers intentionally use the engine's index-only
	// reader. It validates durable committed records without materializing the
	// large detailed-frame payload; detail remains exclusive to the explicit
	// ReadSelectedJournalFrame request below.
	std::uint64_t totalFrameCount = 0u;
	const auto result = TerraEngine::Profiling::ReadProfilerJournalV1IndexPage(
		capturePath, 0u, 0u, outResult, totalFrameCount, outError);
	return result == TerraEngine::Profiling::ProfilerJournalV1ResultCode::Ok
		|| result == TerraEngine::Profiling::ProfilerJournalV1ResultCode::IncompleteTail;
}

bool ProfilerCaptureStore::ReadJournalIndexPage(
	const std::filesystem::path& capturePath,
	std::size_t offset,
	std::size_t pageSize,
	ProfilerCaptureIndexPage& outPage,
	std::string& outError) const
{
	outPage = {};
	if (root.empty() || !IsPathInsideProject(capturePath)
		|| !PathStartsWith(capturePath, GetProfilerDirectory()))
	{
		outError = "Profiler captures can only be read from the opened project's Profiler folder.";
		return false;
	}
	const std::size_t boundedPageSize = std::clamp<std::size_t>(
		pageSize, 1u, kMaximumIndexPageSize);
	const std::uint64_t engineOffset = offset == kNewestIndexPageOffset
		? TerraEngine::Profiling::kProfilerJournalV1NewestIndexPage
		: static_cast<std::uint64_t>(offset);
	TerraEngine::Profiling::ProfilerJournalV1OpenResult journal;
	std::uint64_t totalFrameCount = 0u;
	const auto result = TerraEngine::Profiling::ReadProfilerJournalV1IndexPage(
		capturePath,
		engineOffset,
		static_cast<std::uint32_t>(boundedPageSize),
		journal,
		totalFrameCount,
		outError);
	if (result != TerraEngine::Profiling::ProfilerJournalV1ResultCode::Ok
		&& result != TerraEngine::Profiling::ProfilerJournalV1ResultCode::IncompleteTail)
	{
		return false;
	}
	if (offset == kNewestIndexPageOffset)
	{
		offset = totalFrameCount == 0u ? 0u
			: ((totalFrameCount - 1u) / boundedPageSize) * boundedPageSize;
	}
	offset = static_cast<std::size_t>(std::min<std::uint64_t>(offset, totalFrameCount));

	outPage.metadata = std::move(journal.metadata);
	outPage.summary = journal.summary;
	outPage.offset = offset;
	outPage.pageSize = boundedPageSize;
	outPage.totalFrameCount = static_cast<std::size_t>(totalFrameCount);
	outPage.complete = journal.complete;
	outPage.recoveredIncompleteTail = journal.recoveredIncompleteTail;
	outPage.frames.reserve(journal.index.size());
	for (const auto& entry : journal.index)
	{
		outPage.frames.push_back({entry.frameIndex, entry.durationMs, entry.complete});
	}
	return true;
}

bool ProfilerCaptureStore::ReadSelectedJournalFrame(
	const std::filesystem::path& capturePath,
	std::uint64_t frameIndex,
	TerraEngine::Profiling::ProfilerFrameRecord& outFrame,
	std::string& outError) const
{
	if (root.empty() || !IsPathInsideProject(capturePath) || !PathStartsWith(capturePath, GetProfilerDirectory()))
	{
		outError = "Profiler captures can only be read from the opened project's Profiler folder.";
		return false;
	}
	return TerraEngine::Profiling::ReadProfilerJournalV1SelectedFrame(capturePath, frameIndex, outFrame, outError)
		== TerraEngine::Profiling::ProfilerJournalV1ResultCode::Ok;
}


std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> ProfilerCaptureStore::ListCaptures(std::string& outError, const std::function<bool()>& shouldCancel) const
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

		if (!entry.is_regular_file() || entry.path().extension() != ".teprof")
		{
			continue;
		}

		TerraEngine::Profiling::ProfilerJournalV1OpenResult journal;
		std::uint64_t totalFrameCount = 0u;
		std::string journalError;
		const TerraEngine::Profiling::ProfilerJournalV1ResultCode journalResult =
			TerraEngine::Profiling::ReadProfilerJournalV1IndexPage(
				entry.path(), 0u, 0u, journal, totalFrameCount, journalError);
		if (journalResult == TerraEngine::Profiling::ProfilerJournalV1ResultCode::Ok
			|| journalResult == TerraEngine::Profiling::ProfilerJournalV1ResultCode::IncompleteTail)
		{
			captures.push_back(MakeJournalArtifact(entry.path(), journal));
			continue;
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


std::filesystem::path ProfilerCaptureStore::MakeJournalPath(const TerraEngine::Profiling::ProfilerCaptureMetadata& metadata) const
{
	std::string projectName = metadata.projectName.empty() ? "Capture" : metadata.projectName;

	for (char& character : projectName)
	{
		if (character == '/' || character == '\\' || character == ':' || character == '*' || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
		{
			character = '_';
		}
	}
	return GetProfilerDirectory() / (projectName + "-" + TerraEngine::Profiling::MakeTimestampForFileName() + ".teprof");
}
