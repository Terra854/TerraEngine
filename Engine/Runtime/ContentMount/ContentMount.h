// ContentMount.h — Runtime content-access abstraction for TEPAK archives.
//
// In exported builds the ContentMount replaces the filesystem-based resource access
// with reads from a .tepak archive. The engine's ResourceManager calls into this
// abstraction when the build is running in export mode, transparently routing file
// operations through the TEPAK PackReader.
//
// TEPAK first-party source code is copied into Engine/Runtime/ContentMount/ so the
// engine build is self-contained. TEPAK's third-party dependencies (zstd, xxhash)
// remain sourced from TEPAK/External/ as submodules. The standalone TEPAK/ project
// is untouched.

#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare TEPAK reader to avoid leaking TEPAK internals into the engine
// header. The implementation file includes the concrete TEPAK headers directly.
namespace tepak {
	class PackReader;
}

/// ContentMount provides the engine runtime with a virtualised file-read interface
/// backed by a TEPAK archive. In editor builds the mount is not used — the engine
/// reads directly from the project's filesystem. In export builds, all assets are
/// read through this mount.
class TERRAENGINE_API ContentMount {
public:
	ContentMount();
	~ContentMount();

	ContentMount(ContentMount&&) noexcept;
	ContentMount& operator=(ContentMount&&) noexcept;

	ContentMount(const ContentMount&) = delete;
	ContentMount& operator=(const ContentMount&) = delete;

	/// Opens a .tepak archive and mounts it as the runtime content root.
	/// Throws on failure (invalid archive, missing file, version mismatch).
	/// @param archivePath Absolute path to the .tepak file.
	/// @return true if the archive was successfully opened and validated.
	bool Mount(const std::filesystem::path& archivePath);

	/// Returns whether a content archive is currently mounted.
	[[nodiscard]] bool IsMounted() const;

	/// Returns the path to the mounted archive, or empty if nothing is mounted.
	[[nodiscard]] const std::filesystem::path& GetArchivePath() const;

	/// Checks whether a logical path exists inside the mounted archive.
	/// @param logicalPath Normalised forward-slash path (e.g. "scenes/main.tescene").
	[[nodiscard]] bool Exists(std::string_view logicalPath) const;

	/// Reads the entire contents of a logical file from the mounted archive.
	/// @param logicalPath Normalised forward-slash path.
	/// @return Byte buffer with the file contents.
	[[nodiscard]] std::vector<uint8_t> ReadAll(std::string_view logicalPath) const;

	/// Reads a byte range from a logical file, decompressing only overlapping extents.
	/// @param logicalPath Normalised forward-slash path.
	/// @param offset Byte offset within the logical file.
	/// @param size Number of bytes to read.
	/// @return Byte buffer with the requested range.
	[[nodiscard]] std::vector<uint8_t> ReadRange(std::string_view logicalPath, uint64_t offset, uint64_t size) const;

	/// Releases the mounted archive and frees all associated resources.
	void Unmount();

	/// Global singleton access — the engine keeps one active content mount.
	static ContentMount& Instance();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
