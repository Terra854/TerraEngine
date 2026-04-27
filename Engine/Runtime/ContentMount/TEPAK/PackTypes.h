// PackTypes.h — Contains declarations for the Pack Types constants, type definitions, and lightweight declarations used by public runtime TEPAK reader and archive type declarations.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tepak {

// StorageMode records how one extent was stored inside the pack payload region.
// StorageMode names the supported states explicitly so serialization and switches stay readable.
enum class StorageMode : std::uint32_t {
	Raw = 0,
	Zstd = 1,
	ZstdDictionary = 2,
};

// ValidationSeverity lets tooling distinguish informational notes from failures.
// ValidationSeverity names the supported states explicitly so serialization and switches stay readable.
enum class ValidationSeverity : std::uint32_t {
	Info = 0,
	Warning = 1,
	Error = 2,
};

// PackNodeKind identifies whether a directory listing child is a file or folder.
// PackNodeKind names the supported states explicitly so serialization and switches stay readable.
enum class PackNodeKind : std::uint32_t {
	Directory = 0,
	File = 1,
};

// TEPAK uses this sentinel where a directory does not have a real parent.
inline constexpr std::uint32_t kInvalidDirectoryId = 0xFFFFFFFFU;

// DictionaryProfile explains whether a pack-level zstd dictionary was selected
// and how large the accepted dictionary ended up being.
struct DictionaryProfile final {
	bool dictionaryPresent = false;
	std::uint64_t dictionarySize = 0;
	std::uint64_t dictionaryHash = 0;
	std::string selectionMode;
	double selectionScore = 0.0;
	std::uint32_t compressionEligibleExtentCount = 0;
};

// PackExtent describes one independently decodable byte range inside the pack.
// struct PackExtent groups related state and operations so callers have one documented contract.
struct PackExtent final {
	// fileOffset is the byte offset inside the logical file, while storedOffset
	// is the byte offset inside the archive payload region.
	std::uint64_t fileOffset = 0;
	std::uint64_t storedOffset = 0;
	std::uint64_t storedSize = 0;
	std::uint64_t uncompressedSize = 0;
	std::uint64_t extentHash = 0;
	StorageMode codec = StorageMode::Raw;
	std::uint32_t extentFlags = 0;
};

// DirectoryNode stores one logical directory and the table spans of its children.
// struct DirectoryNode groups related state and operations so callers have one documented contract.
struct DirectoryNode final {
	std::uint32_t directoryId = 0;
	std::uint32_t parentDirectoryId = kInvalidDirectoryId;
	std::string normalizedPath;
	std::uint64_t pathHash = 0;
	std::uint32_t pathOffset = 0;
	std::uint32_t pathLength = 0;
	std::uint32_t firstChildDirectoryIndex = 0;
	std::uint32_t childDirectoryCount = 0;
	std::uint32_t firstChildFileIndex = 0;
	std::uint32_t childFileCount = 0;
};

// DirectoryChild is the lightweight view returned when listing a directory.
// struct DirectoryChild groups related state and operations so callers have one documented contract.
struct DirectoryChild final {
	PackNodeKind kind = PackNodeKind::File;
	std::string normalizedPath;
	std::string name;
	std::uint64_t logicalSize = 0;
};

// DirectoryListingResult contains one directory and its immediate children.
// struct DirectoryListingResult groups related state and operations so callers have one documented contract.
struct DirectoryListingResult final {
	std::string normalizedPath;
	std::vector<DirectoryChild> children;
};

// PackEntry stores one logical file plus the span of extents that reconstruct it.
// struct PackEntry groups related state and operations so callers have one documented contract.
struct PackEntry final {
	std::uint32_t fileId = 0;
	std::uint32_t parentDirectoryId = kInvalidDirectoryId;
	std::string normalizedPath;
	std::string name;
	std::uint64_t pathHash = 0;
	std::uint32_t pathOffset = 0;
	std::uint32_t pathLength = 0;
	std::uint32_t entryFlags = 0;
	std::string assetClass;
	std::uint32_t firstExtentIndex = 0;
	std::uint32_t entryExtentCount = 0;
	std::uint64_t logicalSize = 0;
	std::uint64_t storedSize = 0;
	std::uint64_t entryHash = 0;
};

// PackDescriptor mirrors the fixed archive header so tools do not need to read
// the binary header format directly.
struct PackDescriptor final {
	std::uint64_t archiveIdentifier = 0;
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
	std::uint32_t versionMajor = 0;
	std::uint32_t versionMinor = 0;
};

// PackProgress is emitted by the writer so CLI and editor callers can present
// periodic progress without polling internal worker state.
struct PackProgress final {
	std::uint64_t totalLogicalBytes = 0;
	std::uint64_t processedLogicalBytes = 0;
	std::uint64_t archiveSizeSoFar = 0;
	std::uint32_t totalEntries = 0;
	std::uint32_t completedEntries = 0;
	std::uint32_t totalExtents = 0;
	std::uint32_t completedExtents = 0;
	std::string currentEntryPath;
	double elapsedSeconds = 0.0;
	double estimatedRemainingSeconds = 0.0;
	bool etaAvailable = false;
	bool finalUpdate = false;
};

using PackProgressCallback = std::function<void(const PackProgress&)>;

// PackagingRequest controls offline pack creation. The request is intentionally
// plain data so both the standalone CLI and editor tools can fill it out.
struct PackagingRequest final {
	std::filesystem::path inputRoot;
	std::filesystem::path outputPackPath;
	std::vector<std::string> entrySelectionRules;
	std::string pathNormalizationPolicy = "lower-forward-slash";
	// Offline pack creation can afford larger extents because decode speed is
	// driven more by extent count than by zstd level for this archive format.
	std::uint32_t extentSizingBytes = 1024U * 1024U;
	std::uint32_t workerCount = 1U;
	bool enableDictionaryTraining = true;
	std::uint32_t maxDictionaryBytes = 4U * 1024U * 1024U;
	std::uint64_t peakMemoryBudgetBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
	bool overwriteExisting = false;
	bool deterministicMode = true;
	bool enableCompression = true;
	// Compression level raises export cost, but it keeps runtime decode speed
	// effectively unchanged because TEPAK still decodes independent extents.
	std::int32_t compressionLevel = 9;
	// Callers can subscribe to periodic pack progress without coupling the
	// standalone library to a specific console or UI implementation.
	PackProgressCallback progressCallback;
	// Progress snapshots are throttled so large packs do not flood the caller
	// with one callback per extent.
	std::uint32_t progressUpdateIntervalMs = 1000U;
};

// ValidationFinding records one concrete issue discovered during validation.
// struct ValidationFinding groups related state and operations so callers have one documented contract.
struct ValidationFinding final {
	ValidationSeverity severity = ValidationSeverity::Info;
	std::string category;
	std::string location;
	std::string ruleIdentifier;
	std::string message;
	std::string entryPath;
};

// ValidationReport is the complete result of one validation pass.
// struct ValidationReport groups related state and operations so callers have one documented contract.
struct ValidationReport final {
	std::filesystem::path packPath;
	bool validationStatus = false;
	std::size_t errorCount = 0;
	std::size_t warningCount = 0;
	std::vector<ValidationFinding> findings;
	std::uint32_t checkedEntryCount = 0;
	std::uint32_t checkedDirectoryCount = 0;
	std::uint32_t checkedExtentCount = 0;
	bool integrityChecksPerformed = false;
};

// PackWriteResult returns the metadata that was written along with a few build
// diagnostics that are useful in tools and benchmarks.
struct PackWriteResult final {
	PackDescriptor descriptor;
	DictionaryProfile dictionary;
	std::vector<DirectoryNode> directories;
	std::vector<PackEntry> entries;
	std::vector<PackExtent> extents;
	std::uint32_t workerCountUsed = 1U;
	std::uint64_t estimatedPeakResidentBytes = 0;
};

// PackInspectResult is the lighter metadata bundle returned by PackReader::Inspect.
// struct PackInspectResult groups related state and operations so callers have one documented contract.
struct PackInspectResult final {
	PackDescriptor descriptor;
	DictionaryProfile dictionary;
	std::vector<DirectoryNode> directories;
	std::vector<PackEntry> entries;
};

// ByteBuffer is TEPAK's common owning byte container for metadata and payloads.
using ByteBuffer = std::vector<std::uint8_t>;

} // namespace tepak
