// PackModel.h — TEPAK archive data model
//
// Defines the binary records and in-memory structures used to represent the TEPAK archive format. It includes the definitions for file headers, directory nodes, entry records, and extent plans used throughout the export pipeline.

#pragma once

#include "TEPAK/PackTypes.h"
#include "TEPAK/Version.h"

#include <array>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace tepak::model {

inline constexpr std::array<char, 4> kMagic = {'T', 'P', 'A', 'K'};
inline constexpr std::uint32_t kVersionMajor = Version::kVersionMajor;
inline constexpr std::uint32_t kVersionMinor = Version::kVersionMinor;
inline constexpr std::uint64_t kMetadataAlignment = 8U;

// struct BinaryHeader groups related state and operations so callers have one documented contract.
struct BinaryHeader final {
	std::array<char, 4> magic = kMagic;
	std::uint32_t versionMajor = kVersionMajor;
	std::uint32_t versionMinor = kVersionMinor;
	std::uint32_t headerSize = 0;
	std::uint32_t flags = 0;
	std::uint32_t directoryCount = 0;
	std::uint32_t fileCount = 0;
	std::uint32_t extentCount = 0;
	std::uint32_t defaultExtentSize = 0;
	std::uint32_t rootDirectoryId = 0;
	std::uint64_t pathTableBytes = 0;
	std::uint64_t directoryTableOffset = 0;
	std::uint64_t fileTableOffset = 0;
	std::uint64_t extentTableOffset = 0;
	std::uint64_t pathTableOffset = 0;
	std::uint64_t dictionaryOffset = 0;
	std::uint64_t dictionarySize = 0;
	std::uint64_t dataRegionOffset = 0;
	std::uint64_t archiveSize = 0;
	std::uint64_t dictionaryHash = 0;
	std::uint64_t metadataHash = 0;
	std::uint64_t codecMask = 0;
	std::uint64_t archiveIdentifier = 0;
};

// struct BinaryDirectoryRecord groups related state and operations so callers have one documented contract.
struct BinaryDirectoryRecord final {
	std::uint64_t pathHash = 0;
	std::uint32_t pathOffset = 0;
	std::uint32_t pathLength = 0;
	std::uint32_t parentDirectoryId = kInvalidDirectoryId;
	std::uint32_t firstChildDirectoryIndex = 0;
	std::uint32_t childDirectoryCount = 0;
	std::uint32_t firstChildFileIndex = 0;
	std::uint32_t childFileCount = 0;
};

// struct BinaryEntryRecord groups related state and operations so callers have one documented contract.
struct BinaryEntryRecord final {
	std::uint32_t parentDirectoryId = kInvalidDirectoryId;
	std::uint64_t pathHash = 0;
	std::uint32_t pathOffset = 0;
	std::uint32_t pathLength = 0;
	std::uint32_t entryFlags = 0;
	std::uint32_t assetClass = 0;
	std::uint32_t firstExtentIndex = 0;
	std::uint32_t entryExtentCount = 0;
	std::uint64_t logicalSize = 0;
	std::uint64_t storedSize = 0;
	std::uint64_t entryHash = 0;
};

// struct BinaryExtentRecord groups related state and operations so callers have one documented contract.
struct BinaryExtentRecord final {
	std::uint64_t fileOffset = 0;
	std::uint64_t storedOffset = 0;
	std::uint64_t storedSize = 0;
	std::uint64_t uncompressedSize = 0;
	std::uint64_t extentHash = 0;
	std::uint32_t codec = 0;
	std::uint32_t extentFlags = 0;
};

// struct StagedFile groups related state and operations so callers have one documented contract.
struct StagedFile final {
	std::filesystem::path sourcePath;
	std::string normalizedPath;
	std::string name;
	std::string assetClass;
	std::uint32_t parentDirectoryId = kInvalidDirectoryId;
	std::uint64_t logicalSize = 0;
};

// struct PackagingInventory groups related state and operations so callers have one documented contract.
struct PackagingInventory final {
	std::vector<DirectoryNode> directories;
	std::vector<StagedFile> stagedFiles;
	std::vector<ByteBuffer> dictionarySamples;
	std::uint64_t totalInputBytes = 0;
};

// struct ExtentPlan groups related state and operations so callers have one documented contract.
struct ExtentPlan final {
	std::uint32_t entryIndex = 0;
	std::uint64_t fileOffset = 0;
	std::uint64_t uncompressedSize = 0;
	std::uint64_t storedOffset = 0;
	std::uint64_t storedSize = 0;
	std::uint64_t extentHash = 0;
	StorageMode codec = StorageMode::Raw;
};

// struct CompressedExtentResult groups related state and operations so callers have one documented contract.
struct CompressedExtentResult final {
	ByteBuffer storedBytes;
	StorageMode mode = StorageMode::Raw;
	std::uint64_t extentHash = 0;
	std::uint64_t uncompressedSize = 0;
};

// struct PendingCompressionTask groups related state and operations so callers have one documented contract.
struct PendingCompressionTask final {
	std::uint32_t extentIndex = 0;
	std::uint32_t entryIndex = 0;
	std::future<CompressedExtentResult> future;
};

// struct PackPlan groups related state and operations so callers have one documented contract.
struct PackPlan final {
	PackDescriptor descriptor;
	DictionaryProfile dictionary;
	ByteBuffer dictionaryBytes;
	std::vector<DirectoryNode> directories;
	std::vector<PackEntry> entries;
	std::vector<ExtentPlan> extents;
	std::string pathTable;
	std::uint64_t estimatedPeakResidentBytes = 0;
	std::uint32_t workerCountUsed = 1U;
};

} // namespace tepak::model
