// PackReader.cpp — Contains the Pack Reader read-side state, parsing helpers, and data extraction flow implementation for runtime TEPAK metadata parsing, extent reads, and mounted archive lookup flow.

#include "TEPAK/PackReader.h"

#include "archive/ArchiveWorkflow.h"
#include "io/PathNormalizer.h"
#include "model/PackModel.h"
#include "Utilities/Tepak/BinaryIO.h"

#include <fstream>
#include <stdexcept>

namespace {

// ReadPathFromTable is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ReadPathFromTable(const std::string& pathTable, std::uint32_t offset, std::uint32_t length, const char* recordName) {
	// All paths live in one shared string table, so every record must stay
	// inside that table's bounds before the path can be reconstructed.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (static_cast<std::size_t>(offset + length) > pathTable.size()) {
		throw std::runtime_error(std::string(recordName) + " points outside the path table.");
	}
	return pathTable.substr(offset, length);
}

} // namespace

namespace tepak::archive {

// OpenPackFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
OpenedPack OpenPackFile(const std::filesystem::path& packPath) {
	OpenedPack openedPack;
	openedPack.packPath = packPath;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(packPath)) {
		throw std::runtime_error("The requested pack does not exist.");
	}

	std::ifstream stream(packPath, std::ios::binary);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!stream) {
		throw std::runtime_error("Failed to open the requested pack.");
	}

	const auto header = util::ReadPod<model::BinaryHeader>(stream);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (header.magic != model::kMagic) {
		throw std::runtime_error("The pack magic does not identify a TEPAK archive.");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (header.headerSize != sizeof(model::BinaryHeader)) {
		throw std::runtime_error("The pack header size is invalid.");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (header.versionMajor != model::kVersionMajor || header.versionMinor != model::kVersionMinor) {
		throw std::runtime_error("The pack version does not match the supported TEPAK layout.");
	}

	const auto archiveSize = std::filesystem::file_size(packPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (header.archiveSize > archiveSize || header.dataRegionOffset > archiveSize) {
		throw std::runtime_error("The pack declares offsets outside the archive size.");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (header.directoryTableOffset > header.fileTableOffset || header.fileTableOffset > header.extentTableOffset || header.extentTableOffset > header.pathTableOffset) {
		throw std::runtime_error("The pack metadata offsets are not ordered correctly.");
	}

	// Copy the fixed header into the public descriptor first so later code can
	// reason about one normalized in-memory representation of the archive.
	openedPack.descriptor.archiveIdentifier = header.archiveIdentifier;
	openedPack.descriptor.headerSize = header.headerSize;
	openedPack.descriptor.flags = header.flags;
	openedPack.descriptor.directoryCount = header.directoryCount;
	openedPack.descriptor.fileCount = header.fileCount;
	openedPack.descriptor.extentCount = header.extentCount;
	openedPack.descriptor.defaultExtentSize = header.defaultExtentSize;
	openedPack.descriptor.rootDirectoryId = header.rootDirectoryId;
	openedPack.descriptor.pathTableBytes = header.pathTableBytes;
	openedPack.descriptor.directoryTableOffset = header.directoryTableOffset;
	openedPack.descriptor.fileTableOffset = header.fileTableOffset;
	openedPack.descriptor.extentTableOffset = header.extentTableOffset;
	openedPack.descriptor.pathTableOffset = header.pathTableOffset;
	openedPack.descriptor.dictionaryOffset = header.dictionaryOffset;
	openedPack.descriptor.dictionarySize = header.dictionarySize;
	openedPack.descriptor.dataRegionOffset = header.dataRegionOffset;
	openedPack.descriptor.archiveSize = header.archiveSize;
	openedPack.descriptor.dictionaryHash = header.dictionaryHash;
	openedPack.descriptor.metadataHash = header.metadataHash;
	openedPack.descriptor.codecMask = header.codecMask;
	openedPack.descriptor.versionMajor = header.versionMajor;
	openedPack.descriptor.versionMinor = header.versionMinor;

	stream.seekg(static_cast<std::streamoff>(header.pathTableOffset), std::ios::beg);
	openedPack.pathTable.resize(header.pathTableBytes);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (header.pathTableBytes > 0U) {
		stream.read(openedPack.pathTable.data(), static_cast<std::streamsize>(header.pathTableBytes));
	}

	stream.seekg(static_cast<std::streamoff>(header.dictionaryOffset), std::ios::beg);
	openedPack.dictionaryBytes.resize(header.dictionarySize);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (header.dictionarySize > 0U) {
		stream.read(reinterpret_cast<char*>(openedPack.dictionaryBytes.data()), static_cast<std::streamsize>(header.dictionarySize));
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!openedPack.dictionaryBytes.empty()) {
		// The opened pack keeps one prepared dictionary alive so every extent
		// decode can reuse the digested tables instead of rebuilding them.
		const codec::CodecProvider codecProvider;
		openedPack.preparedDictionary = codecProvider.PrepareDecompressionDictionary(openedPack.dictionaryBytes);
	}

	openedPack.dictionary.dictionaryPresent = archive::HasDictionaryFlag(header.flags);
	openedPack.dictionary.dictionarySize = header.dictionarySize;
	openedPack.dictionary.dictionaryHash = header.dictionaryHash;
	openedPack.dictionary.selectionMode = openedPack.dictionary.dictionaryPresent ? "trained" : "none";

	stream.seekg(static_cast<std::streamoff>(header.directoryTableOffset), std::ios::beg);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t index = 0U; index < header.directoryCount; ++index) {
		const auto record = util::ReadPod<model::BinaryDirectoryRecord>(stream);
		DirectoryNode node;
		node.directoryId = index;
		node.parentDirectoryId = record.parentDirectoryId;
		node.pathHash = record.pathHash;
		node.pathOffset = record.pathOffset;
		node.pathLength = record.pathLength;
		node.firstChildDirectoryIndex = record.firstChildDirectoryIndex;
		node.childDirectoryCount = record.childDirectoryCount;
		node.firstChildFileIndex = record.firstChildFileIndex;
		node.childFileCount = record.childFileCount;
		node.normalizedPath = ReadPathFromTable(openedPack.pathTable, node.pathOffset, node.pathLength, "A directory record");
		openedPack.directories.push_back(std::move(node));
	}

	stream.seekg(static_cast<std::streamoff>(header.fileTableOffset), std::ios::beg);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t index = 0U; index < header.fileCount; ++index) {
		const auto record = util::ReadPod<model::BinaryEntryRecord>(stream);
		PackEntry entry;
		entry.fileId = index;
		entry.parentDirectoryId = record.parentDirectoryId;
		entry.pathHash = record.pathHash;
		entry.pathOffset = record.pathOffset;
		entry.pathLength = record.pathLength;
		entry.entryFlags = record.entryFlags;
		entry.assetClass = DecodeAssetClass(record.assetClass);
		entry.firstExtentIndex = record.firstExtentIndex;
		entry.entryExtentCount = record.entryExtentCount;
		entry.logicalSize = record.logicalSize;
		entry.storedSize = record.storedSize;
		entry.entryHash = record.entryHash;
		entry.normalizedPath = ReadPathFromTable(openedPack.pathTable, entry.pathOffset, entry.pathLength, "A file record");
		entry.name = io::ExtractLeafName(entry.normalizedPath);
		openedPack.entries.push_back(std::move(entry));
	}

	stream.seekg(static_cast<std::streamoff>(header.extentTableOffset), std::ios::beg);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t index = 0U; index < header.extentCount; ++index) {
		const auto record = util::ReadPod<model::BinaryExtentRecord>(stream);
		PackExtent extent;
		extent.fileOffset = record.fileOffset;
		extent.storedOffset = record.storedOffset;
		extent.storedSize = record.storedSize;
		extent.uncompressedSize = record.uncompressedSize;
		extent.extentHash = record.extentHash;
		extent.codec = static_cast<StorageMode>(record.codec);
		extent.extentFlags = record.extentFlags;
		openedPack.extents.push_back(std::move(extent));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (openedPack.directories.empty()) {
		throw std::runtime_error("The pack does not contain a root directory.");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (openedPack.descriptor.rootDirectoryId >= openedPack.directories.size()) {
		throw std::runtime_error("The pack root directory identifier is invalid.");
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& directory : openedPack.directories) {
		if (directory.childDirectoryCount > 0U &&
			(static_cast<std::uint64_t>(directory.firstChildDirectoryIndex) + directory.childDirectoryCount) > openedPack.directories.size()) {
			throw std::runtime_error("A directory child span points outside the directory table.");
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (directory.childFileCount > 0U &&
			(static_cast<std::uint64_t>(directory.firstChildFileIndex) + directory.childFileCount) > openedPack.entries.size()) {
			throw std::runtime_error("A directory child span points outside the file table.");
		}
		openedPack.directoryLookup.emplace(directory.normalizedPath, directory.directoryId);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : openedPack.entries) {
		if (entry.parentDirectoryId >= openedPack.directories.size()) {
			throw std::runtime_error("A file entry points at an invalid parent directory.");
		}
		openedPack.fileLookup.emplace(entry.normalizedPath, entry.fileId);
	}

	// Lookup maps let later list and read operations resolve logical paths in
	// near-constant time instead of rescanning the metadata tables.
	return openedPack;
}

} // namespace tepak::archive

namespace tepak {

// struct PackReader groups related state and operations so callers have one documented contract.
struct PackReader::Impl final {
	archive::OpenedPack openedPack;
};

PackReader::PackReader() = default;
PackReader::~PackReader() = default;
PackReader::PackReader(PackReader&&) noexcept = default;
PackReader& PackReader::operator=(PackReader&&) noexcept = default;

// PackReader::Open is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
PackReader PackReader::Open(const std::filesystem::path& packPath) {
	PackReader reader;
	reader.m_impl = std::make_shared<Impl>();
	reader.m_impl->openedPack = archive::OpenPackFile(packPath);
	return reader;
}

// PackReader::GetDescriptor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const PackDescriptor& PackReader::GetDescriptor() const {
	return m_impl->openedPack.descriptor;
}

// PackReader::GetDictionary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const DictionaryProfile& PackReader::GetDictionary() const {
	return m_impl->openedPack.dictionary;
}

// PackReader::GetDirectories is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const std::vector<DirectoryNode>& PackReader::GetDirectories() const {
	return m_impl->openedPack.directories;
}

// PackReader::GetEntries is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const std::vector<PackEntry>& PackReader::GetEntries() const {
	return m_impl->openedPack.entries;
}

// PackReader::Inspect is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
PackInspectResult PackReader::Inspect() const {
	return {m_impl->openedPack.descriptor, m_impl->openedPack.dictionary, m_impl->openedPack.directories, m_impl->openedPack.entries};
}

// PackReader::ListDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
DirectoryListingResult PackReader::ListDirectory(std::string_view logicalDirectory) const {
	const auto normalizedDirectory = io::NormalizeLookupPath(logicalDirectory);
	const auto directoryIterator = m_impl->openedPack.directoryLookup.find(normalizedDirectory);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (directoryIterator == m_impl->openedPack.directoryLookup.end()) {
		throw std::runtime_error("The requested directory is not present in the pack.");
	}

	const auto& directory = m_impl->openedPack.directories.at(directoryIterator->second);
	DirectoryListingResult result;
	result.normalizedPath = directory.normalizedPath;
	result.children.reserve(static_cast<std::size_t>(directory.childDirectoryCount + directory.childFileCount));

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t index = 0U; index < directory.childDirectoryCount; ++index) {
		const auto& childDirectory = m_impl->openedPack.directories.at(directory.firstChildDirectoryIndex + index);
		DirectoryChild child;
		child.kind = PackNodeKind::Directory;
		child.normalizedPath = childDirectory.normalizedPath;
		child.name = io::ExtractLeafName(childDirectory.normalizedPath);
		result.children.push_back(std::move(child));
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t index = 0U; index < directory.childFileCount; ++index) {
		const auto& childEntry = m_impl->openedPack.entries.at(directory.firstChildFileIndex + index);
		DirectoryChild child;
		child.kind = PackNodeKind::File;
		child.normalizedPath = childEntry.normalizedPath;
		child.name = childEntry.name;
		child.logicalSize = childEntry.logicalSize;
		result.children.push_back(std::move(child));
	}

	return result;
}

// PackReader::ReadAll is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ByteBuffer PackReader::ReadAll(std::string_view logicalPath) const {
	const auto normalizedPath = io::NormalizeLookupPath(logicalPath);
	const auto found = m_impl->openedPack.fileLookup.find(normalizedPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (found == m_impl->openedPack.fileLookup.end()) {
		throw std::runtime_error("The requested path is not present in the pack.");
	}

	const auto& entry = m_impl->openedPack.entries.at(found->second);
	return archive::ReadLogicalBytes(m_impl->openedPack, normalizedPath, 0U, entry.logicalSize);
}

// PackReader::ReadRange is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ByteBuffer PackReader::ReadRange(std::string_view logicalPath, std::uint64_t offset, std::uint64_t size) const {
	return archive::ReadLogicalBytes(m_impl->openedPack, logicalPath, offset, size);
}

// PackReader::Extract is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void PackReader::Extract(std::string_view logicalPath, const std::filesystem::path& outputDirectory, std::uint32_t workerCount) const {
	archive::ExtractRequest request;
	request.normalizedPath = io::NormalizeLookupPath(logicalPath);
	request.outputDirectory = outputDirectory;
	request.workerCount = workerCount;
	archive::ExtractLogicalPath(m_impl->openedPack, request);
}

} // namespace tepak
