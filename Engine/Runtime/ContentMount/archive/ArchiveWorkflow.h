// ArchiveWorkflow.h — Contains declarations for the Archive Workflow file-specific types, helper functions, and module integration code used by runtime TEPAK metadata parsing, extent reads, and mounted archive lookup flow.

#pragma once

#include "TEPAK/PackTypes.h"
#include "codec/CodecProvider.h"
#include "model/PackModel.h"
#include "util/WorkerPool.h"

#include <filesystem>
#include <future>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tepak::archive {

// OpenedPack keeps every table needed for random access after one initial load.
// The prepared dictionary is cached here so repeated reads do not rebuild zstd
// dictionary state for every extent.
struct OpenedPack final {
	OpenedPack() = default;
	OpenedPack(OpenedPack&&) noexcept = default;
	OpenedPack& operator=(OpenedPack&&) noexcept = default;
	OpenedPack(const OpenedPack&) = delete;
	OpenedPack& operator=(const OpenedPack&) = delete;

	std::filesystem::path packPath;
	PackDescriptor descriptor;
	DictionaryProfile dictionary;
	std::vector<DirectoryNode> directories;
	std::vector<PackEntry> entries;
	std::vector<PackExtent> extents;
	ByteBuffer dictionaryBytes;
	codec::PreparedDecompressionDictionary preparedDictionary;
	std::string pathTable;
	std::unordered_map<std::string, std::uint32_t> directoryLookup;
	std::unordered_map<std::string, std::uint32_t> fileLookup;
};

// StreamedWriteState tracks the moving payload cursor while background worker
// tasks finish compressing extents in order.
struct StreamedWriteState final {
	std::uint64_t nextStoredOffset = 0;
	std::vector<model::PendingCompressionTask> pendingTasks;
};

// ExtractRequest normalizes the public PackReader arguments into one internal
// structure that archive helpers can pass around.
struct ExtractRequest final {
	std::string normalizedPath;
	std::filesystem::path outputDirectory;
	std::uint32_t workerCount = 1U;
};

// Discovers files and the logical directory tree that will feed the pack plan.
model::PackagingInventory BuildPackagingInventory(const PackagingRequest& request);
// Converts discovered files into immutable metadata tables and extent plans.
model::PackPlan BuildPackPlan(const PackagingRequest& request, const model::PackagingInventory& inventory, const codec::CodecProvider& codecProvider);

// Opens and validates the metadata tables of an existing pack.
OpenedPack OpenPackFile(const std::filesystem::path& packPath);
// Reads one logical file or byte range by reconstructing the required extents.
ByteBuffer ReadLogicalBytes(const OpenedPack& openedPack, std::string_view logicalPath, std::uint64_t offset, std::uint64_t size);
// Extracts one file or directory subtree from an opened pack.
void ExtractLogicalPath(const OpenedPack& openedPack, const ExtractRequest& request);

// Archive metadata stores compact numeric codes for common asset classes.
std::uint32_t EncodeAssetClass(std::string_view assetClass);
std::string DecodeAssetClass(std::uint32_t assetClassCode);
// Pack flags currently only record whether a shared dictionary is present.
std::uint32_t EncodePackFlags(bool dictionaryPresent);
bool HasDictionaryFlag(std::uint32_t flags);

} // namespace tepak::archive
