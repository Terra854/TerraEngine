// PackReader.h — Public TEPAK archive reader interface
//
// Defines the high-level interface for reading and inspecting TEPAK archives. It provides the mechanisms for logical path resolution, directory listing, and random-access reading of archived content, utilizing an internal implementation to hide archive-specific metadata handling.

#pragma once

#include "TEPAK/PackTypes.h"

#include <memory>
#include <string_view>

namespace tepak {

// PackReader opens one archive and keeps the parsed metadata alive so callers
// can inspect, list, extract, and random-read entries without reparsing the
// header tables on every operation.
class PackReader final {
public:
	PackReader();
	~PackReader();

	PackReader(PackReader&&) noexcept;
	PackReader& operator=(PackReader&&) noexcept;

	PackReader(const PackReader&) = delete;
	PackReader& operator=(const PackReader&) = delete;

	// Opens an existing pack file and validates the top-level metadata tables
	// before exposing any read operations to the caller.
	static PackReader Open(const std::filesystem::path& packPath);

	// Returns the immutable archive descriptor exactly as it was read from disk.
	const PackDescriptor& GetDescriptor() const;
	// Returns dictionary-selection metadata recorded when the pack was built.
	const DictionaryProfile& GetDictionary() const;
	// Returns the directory table in logical-path order.
	const std::vector<DirectoryNode>& GetDirectories() const;
	// Returns the file entry table in logical-path order.
	const std::vector<PackEntry>& GetEntries() const;
	// Returns the metadata needed by "inspect" style tools in one value object.
	PackInspectResult Inspect() const;
	// Lists one logical directory and its immediate children.
	DirectoryListingResult ListDirectory(std::string_view logicalDirectory = {}) const;
	// Reads and reconstructs one full logical file.
	ByteBuffer ReadAll(std::string_view logicalPath) const;
	// Reads a byte range from one logical file, decompressing only the extents
	// that overlap the requested range.
	ByteBuffer ReadRange(std::string_view logicalPath, std::uint64_t offset, std::uint64_t size) const;
	// Extracts a file or a whole subtree back to the filesystem.
	void Extract(std::string_view logicalPath, const std::filesystem::path& outputDirectory, std::uint32_t workerCount = 1U) const;

private:

	struct Impl;
	std::shared_ptr<Impl> m_impl;
};

} // namespace tepak
