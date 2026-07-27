// ContentMount.h - thread-safe runtime access to one mounted TEPAK archive.

#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Checked reads distinguish an empty file from an unavailable or rejected read.
// Legacy vector-returning helpers remain available for existing resource callers.
enum class ContentReadStatus : std::uint8_t
{
	Ok,
	NotMounted,
	NotFound,
	OutOfRange,
	ReadLimitExceeded,
	ArchiveError
};

// This copied metadata record never exposes PackReader-owned references across a
// remount boundary. Callers can safely retain it after the archive changes.
struct TERRAENGINE_API MountedContentEntry final
{
	std::string logicalPath;
	std::string assetClass;
	std::uint64_t logicalSize = 0U;
	std::uint64_t entryHash = 0U;
};

struct TERRAENGINE_API MountedContentReadResult final
{
	ContentReadStatus status = ContentReadStatus::NotMounted;
	MountedContentEntry entry;
	std::vector<std::uint8_t> bytes;

	[[nodiscard]] bool Succeeded() const noexcept { return status == ContentReadStatus::Ok; }
};

// ContentMount publishes complete reader/metadata snapshots under one lock. A
// failed remount leaves the prior snapshot live, while unmount waits for active
// reads so PackReader state is never invalidated underneath worker jobs.
class TERRAENGINE_API ContentMount final
{
public:
	ContentMount();
	~ContentMount();

	ContentMount(ContentMount&&) noexcept;
	ContentMount& operator=(ContentMount&&) noexcept;

	ContentMount(const ContentMount&) = delete;
	ContentMount& operator=(const ContentMount&) = delete;

	// Opens and validates a candidate before atomically replacing the live mount.
	// Returns false without disturbing an existing mount when opening fails.
	bool Mount(const std::filesystem::path& archivePath);

	[[nodiscard]] bool IsMounted() const;
	[[nodiscard]] std::filesystem::path GetArchivePath() const;
	[[nodiscard]] bool Exists(std::string_view logicalPath) const;
	[[nodiscard]] std::optional<MountedContentEntry> FindEntry(std::string_view logicalPath) const;
	[[nodiscard]] std::vector<MountedContentEntry> SnapshotEntries() const;

	// Applies the one lookup policy shared by archive metadata and runtime reads.
	[[nodiscard]] static std::string NormalizeLogicalPath(std::string_view logicalPath);

	// Holds a shared mount lock across metadata validation and PackReader I/O. The
	// caller-provided limit prevents untrusted archive metadata from causing an
	// unexpectedly large allocation before a format reader validates its header.
	[[nodiscard]] MountedContentReadResult ReadRangeChecked(
		std::string_view logicalPath,
		std::uint64_t offset,
		std::uint64_t size,
		std::uint64_t maximumReadBytes = std::numeric_limits<std::uint64_t>::max()) const;
	[[nodiscard]] MountedContentReadResult ReadAllChecked(
		std::string_view logicalPath,
		std::uint64_t maximumReadBytes = std::numeric_limits<std::uint64_t>::max()) const;

	// Compatibility helpers report failures as empty buffers, matching the
	// original ContentMount contract used by existing resource managers.
	[[nodiscard]] std::vector<std::uint8_t> ReadAll(std::string_view logicalPath) const;
	[[nodiscard]] std::vector<std::uint8_t> ReadRange(
		std::string_view logicalPath,
		std::uint64_t offset,
		std::uint64_t size) const;

	void Unmount();
	static ContentMount& Instance();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
