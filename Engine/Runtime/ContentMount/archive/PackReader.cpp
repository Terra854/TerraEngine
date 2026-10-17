// PackReader.cpp — Contains the Pack Reader read-side state, parsing helpers, and data extraction flow implementation for runtime TEPAK metadata parsing, extent reads, and mounted archive lookup flow.

#include "TEPAK/PackReader.h"

#include "archive/ArchiveWorkflow.h"
#include "io/PathNormalizer.h"
#include "model/PackModel.h"
#include "Utilities/Tepak/BinaryIO.h"

#include <fstream>
#include <array>
#include <memory>
#include "xxhash.h"
#include <limits>
#include <unordered_set>
#include <stdexcept>

namespace {


std::string ReadPathFromTable(const std::string& pathTable, std::uint32_t offset, std::uint32_t length, const char* recordName) {
	// All paths live in one shared string table, so every record must stay
	// inside that table's bounds before the path can be reconstructed.

	if (offset > pathTable.size() || length > pathTable.size() - offset) {
		throw std::runtime_error(std::string(recordName) + " points outside the path table.");
	}
	return pathTable.substr(offset, length);
}

} // namespace

namespace tepak::archive {


OpenedPack OpenPackFile(const std::filesystem::path& packPath) {
	OpenedPack openedPack;
	openedPack.packPath = packPath;


	if (!std::filesystem::exists(packPath)) {
		throw std::runtime_error("The requested pack does not exist.");
	}

	std::ifstream stream(packPath, std::ios::binary);

	if (!stream) {
		throw std::runtime_error("Failed to open the requested pack.");
	}

	const auto header = util::ReadPod<model::BinaryHeader>(stream);

	if (header.magic != model::kMagic) {
		throw std::runtime_error("The pack magic does not identify a TEPAK archive.");
	}

	if (header.headerSize != sizeof(model::BinaryHeader)) {
		throw std::runtime_error("The pack header size is invalid.");
	}

	if (header.versionMajor != model::kVersionMajor || header.versionMinor != model::kVersionMinor) {
		throw std::runtime_error("The pack version does not match the supported TEPAK layout.");
	}

	const auto archiveSize = std::filesystem::file_size(packPath);

	if (header.archiveSize > archiveSize || header.dataRegionOffset > archiveSize) {
		throw std::runtime_error("The pack declares offsets outside the archive size.");
	}

	if (header.directoryTableOffset > header.fileTableOffset || header.fileTableOffset > header.extentTableOffset || header.extentTableOffset > header.pathTableOffset) {
		throw std::runtime_error("The pack metadata offsets are not ordered correctly.");
	}

	// Reject impossible sections before trusting counts for allocation or iteration.
	// Each interval is checked by subtraction, so hostile additions cannot wrap.
	const auto fits = [](std::uint64_t offset, std::uint64_t length, std::uint64_t end) {
		return offset <= end && length <= end - offset;
	};
	const auto tableFits = [&](std::uint64_t offset, std::uint64_t count, std::uint64_t stride, std::uint64_t end) {
		return offset <= end && count <= (end - offset) / stride;
	};
	const auto supportedSize = [](std::uint64_t size) {
		return size <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) &&
			size <= static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) &&
			size <= static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max());
	};
	if(header.archiveSize < sizeof(header) || header.archiveSize > archiveSize || !supportedSize(header.archiveSize) ||
		header.directoryTableOffset < sizeof(header) || header.pathTableOffset > header.dictionaryOffset ||
		header.dictionaryOffset > header.dataRegionOffset || header.dataRegionOffset > header.archiveSize ||
		!tableFits(header.directoryTableOffset, header.directoryCount, sizeof(model::BinaryDirectoryRecord), header.fileTableOffset) ||
		!tableFits(header.fileTableOffset, header.fileCount, sizeof(model::BinaryEntryRecord), header.extentTableOffset) ||
		!tableFits(header.extentTableOffset, header.extentCount, sizeof(model::BinaryExtentRecord), header.pathTableOffset) ||
		!fits(header.pathTableOffset, header.pathTableBytes, header.dictionaryOffset) ||
		!fits(header.dictionaryOffset, header.dictionarySize, header.dataRegionOffset) ||
		header.directoryCount == 0 || header.rootDirectoryId != 0) {
		throw std::runtime_error("The pack metadata sections exceed their physical intervals.");
	}

	// Hash the exact serialized table bytes, including record-internal padding,
	// but excluding header, alignment padding, dictionary, and extent payloads.
	std::unique_ptr<XXH3_state_t,decltype(&XXH3_freeState)> metadataState(XXH3_createState(),&XXH3_freeState);
	if(!metadataState || XXH3_64bits_reset(metadataState.get())!=XXH_OK) throw std::runtime_error("Could not initialize archive metadata hash.");
	std::array<std::uint8_t,64*1024> chunk{};
	const auto hashSection=[&](std::uint64_t offset,std::uint64_t length) {
		stream.seekg(static_cast<std::streamoff>(offset),std::ios::beg);
		while(length!=0) {
			const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(length,chunk.size()));
			stream.read(reinterpret_cast<char*>(chunk.data()),static_cast<std::streamsize>(count));
			if(!stream || XXH3_64bits_update(metadataState.get(),chunk.data(),count)!=XXH_OK) throw std::runtime_error("Archive metadata bytes could not be verified.");
			length-=count;
		}
	};
	hashSection(header.directoryTableOffset,static_cast<std::uint64_t>(header.directoryCount)*sizeof(model::BinaryDirectoryRecord));
	hashSection(header.fileTableOffset,static_cast<std::uint64_t>(header.fileCount)*sizeof(model::BinaryEntryRecord));
	hashSection(header.extentTableOffset,static_cast<std::uint64_t>(header.extentCount)*sizeof(model::BinaryExtentRecord));
	hashSection(header.pathTableOffset,header.pathTableBytes);
	if(XXH3_64bits_digest(metadataState.get())!=header.metadataHash) throw std::runtime_error("Archive metadata hash mismatch.");
	if((header.flags & ~1U)!=0 || HasDictionaryFlag(header.flags)!=(header.dictionarySize!=0)) throw std::runtime_error("Archive dictionary declarations disagree.");

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

	if (header.pathTableBytes > 0U) {
		stream.read(openedPack.pathTable.data(), static_cast<std::streamsize>(header.pathTableBytes));
		if(!stream) throw std::runtime_error("The pack path table is truncated.");
	}

	stream.seekg(static_cast<std::streamoff>(header.dictionaryOffset), std::ios::beg);
	openedPack.dictionaryBytes.resize(header.dictionarySize);

	if (header.dictionarySize > 0U) {
		stream.read(reinterpret_cast<char*>(openedPack.dictionaryBytes.data()), static_cast<std::streamsize>(header.dictionarySize));
		if(!stream) throw std::runtime_error("The pack dictionary is truncated.");
	}

	const codec::CodecProvider dictionaryHasher;
	if(dictionaryHasher.HashBytes(openedPack.dictionaryBytes)!=header.dictionaryHash) throw std::runtime_error("Archive dictionary hash mismatch.");
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


	if (openedPack.directories.empty()) {
		throw std::runtime_error("The pack does not contain a root directory.");
	}

	if (openedPack.descriptor.rootDirectoryId >= openedPack.directories.size()) {
		throw std::runtime_error("The pack root directory identifier is invalid.");
	}


	for (const auto& directory : openedPack.directories) {
		if (directory.childDirectoryCount > 0U &&
			(static_cast<std::uint64_t>(directory.firstChildDirectoryIndex) + directory.childDirectoryCount) > openedPack.directories.size()) {
			throw std::runtime_error("A directory child span points outside the directory table.");
		}

		if (directory.childFileCount > 0U &&
			(static_cast<std::uint64_t>(directory.firstChildFileIndex) + directory.childFileCount) > openedPack.entries.size()) {
			throw std::runtime_error("A directory child span points outside the file table.");
		}
		openedPack.directoryLookup.emplace(directory.normalizedPath, directory.directoryId);
	}


	for (const auto& entry : openedPack.entries) {
		if (entry.parentDirectoryId >= openedPack.directories.size()) {
			throw std::runtime_error("A file entry points at an invalid parent directory.");
		}
		openedPack.fileLookup.emplace(entry.normalizedPath, entry.fileId);
	}

	// Validate persisted paths rather than applying lookup repair to metadata.
	std::unordered_set<std::string> pathIdentities;
	for(const auto& directory:openedPack.directories) {
		io::ValidateStoredPath(directory.normalizedPath,directory.directoryId==0);
		if(dictionaryHasher.HashBytes(directory.normalizedPath)!=directory.pathHash) throw std::runtime_error("Archive directory path hash mismatch.");
		if(!pathIdentities.insert(io::StoredPathIdentity(directory.normalizedPath)).second) throw std::runtime_error("Archive paths collide.");
	}
	for(const auto& entry:openedPack.entries) {
		io::ValidateStoredPath(entry.normalizedPath,false);
		if(dictionaryHasher.HashBytes(entry.normalizedPath)!=entry.pathHash) throw std::runtime_error("Archive file path hash mismatch.");
		if(!pathIdentities.insert(io::StoredPathIdentity(entry.normalizedPath)).second) throw std::runtime_error("Archive paths collide.");
	}

	// Iterative ownership traversal bounds malformed cycles and deep trees alike.
	// Mark on enqueue, so duplicate edges are rejected before they can be revisited.
	const auto& root=openedPack.directories.front();
	if(!root.normalizedPath.empty() || root.parentDirectoryId!=kInvalidDirectoryId) throw std::runtime_error("Archive root representation is invalid.");
	const auto parentPath=[](std::string_view path) { const auto slash=path.rfind('/'); return slash==std::string_view::npos ? std::string_view{} : path.substr(0,slash); };
	std::vector<bool> directoryOwned(openedPack.directories.size()), fileOwned(openedPack.entries.size());
	std::vector<std::uint32_t> pending{0}; directoryOwned[0]=true;
	while(!pending.empty()) {
		const auto id=pending.back(); pending.pop_back(); const auto& directory=openedPack.directories[id];
		if(directory.firstChildDirectoryIndex>directoryOwned.size() || directory.childDirectoryCount>directoryOwned.size()-directory.firstChildDirectoryIndex ||
			directory.firstChildFileIndex>fileOwned.size() || directory.childFileCount>fileOwned.size()-directory.firstChildFileIndex) throw std::runtime_error("Archive child span is invalid.");
		for(std::uint32_t index=0; index<directory.childDirectoryCount; ++index) {
			const auto childId=directory.firstChildDirectoryIndex+index; const auto& child=openedPack.directories[childId];
			if(directoryOwned[childId] || child.parentDirectoryId!=id || child.normalizedPath.empty() || parentPath(child.normalizedPath)!=directory.normalizedPath) throw std::runtime_error("Archive directory has cyclic, duplicate, or inconsistent ownership.");
			directoryOwned[childId]=true; pending.push_back(childId);
		}
		for(std::uint32_t index=0; index<directory.childFileCount; ++index) {
			const auto childId=directory.firstChildFileIndex+index; const auto& child=openedPack.entries[childId];
			if(fileOwned[childId] || child.parentDirectoryId!=id || parentPath(child.normalizedPath)!=directory.normalizedPath) throw std::runtime_error("Archive file has duplicate or inconsistent ownership.");
			fileOwned[childId]=true;
		}
	}
	if(std::find(directoryOwned.begin(),directoryOwned.end(),false)!=directoryOwned.end() || std::find(fileOwned.begin(),fileOwned.end(),false)!=fileOwned.end()) throw std::runtime_error("Archive contains unreachable records.");

	// Every entry owns a contiguous, unique run in writer order. Empty files
	// retain the writer's single zero extent rather than bypassing validation.
	std::uint64_t nextExtent = 0;
	for(const auto& entry : openedPack.entries) {
		if(entry.firstExtentIndex != nextExtent || entry.entryExtentCount == 0 ||
			nextExtent > openedPack.extents.size() || entry.entryExtentCount > openedPack.extents.size() - nextExtent ||
			!supportedSize(entry.logicalSize) || !supportedSize(entry.storedSize) ||
			(entry.logicalSize == 0 && entry.entryExtentCount != 1)) {
			throw std::runtime_error("A file has invalid or multiply owned extent references.");
		}
		std::uint64_t logicalEnd = 0, storedTotal = 0;
		for(std::uint32_t index = 0; index < entry.entryExtentCount; ++index) {
			const auto& extent = openedPack.extents.at(static_cast<std::size_t>(nextExtent + index));
			if(extent.fileOffset != logicalEnd || !fits(logicalEnd, extent.uncompressedSize, entry.logicalSize) ||
				(entry.logicalSize != 0 && extent.uncompressedSize == 0) || !supportedSize(extent.uncompressedSize) || !supportedSize(extent.storedSize) ||
				extent.storedOffset < header.dataRegionOffset || !fits(extent.storedOffset, extent.storedSize, header.archiveSize) ||
				!fits(storedTotal, extent.storedSize, entry.storedSize) ||
				(extent.codec != StorageMode::Raw && extent.codec != StorageMode::Zstd && extent.codec != StorageMode::ZstdDictionary) ||
				(extent.codec == StorageMode::Raw && extent.storedSize != extent.uncompressedSize) ||
				(extent.codec != StorageMode::Raw && extent.storedSize == 0) ||
				(extent.codec == StorageMode::ZstdDictionary && (!openedPack.dictionary.dictionaryPresent || openedPack.dictionaryBytes.empty()))) {
				throw std::runtime_error("A file extent has invalid coverage, codec, or physical bytes.");
			}
			logicalEnd += extent.uncompressedSize; storedTotal += extent.storedSize;
		}
		if(logicalEnd != entry.logicalSize || storedTotal != entry.storedSize) throw std::runtime_error("File extent coverage is incomplete.");
		nextExtent += entry.entryExtentCount;
	}
	if(nextExtent != openedPack.extents.size()) throw std::runtime_error("The extent table contains unowned records.");

	// Lookup maps let later list and read operations resolve logical paths in
	// near-constant time instead of rescanning the metadata tables.
	return openedPack;
}

} // namespace tepak::archive

namespace tepak {


struct PackReader::Impl final {
	archive::OpenedPack openedPack;
};

PackReader::PackReader() = default;
PackReader::~PackReader() = default;
PackReader::PackReader(PackReader&&) noexcept = default;
PackReader& PackReader::operator=(PackReader&&) noexcept = default;


PackReader PackReader::Open(const std::filesystem::path& packPath) {
	PackReader reader;
	reader.m_impl = std::make_shared<Impl>();
	reader.m_impl->openedPack = archive::OpenPackFile(packPath);
	return reader;
}


const PackDescriptor& PackReader::GetDescriptor() const {
	return m_impl->openedPack.descriptor;
}


const DictionaryProfile& PackReader::GetDictionary() const {
	return m_impl->openedPack.dictionary;
}


const std::vector<DirectoryNode>& PackReader::GetDirectories() const {
	return m_impl->openedPack.directories;
}


const std::vector<PackEntry>& PackReader::GetEntries() const {
	return m_impl->openedPack.entries;
}


PackInspectResult PackReader::Inspect() const {
	return {m_impl->openedPack.descriptor, m_impl->openedPack.dictionary, m_impl->openedPack.directories, m_impl->openedPack.entries};
}


DirectoryListingResult PackReader::ListDirectory(std::string_view logicalDirectory) const {
	const auto normalizedDirectory = io::NormalizeLookupPath(logicalDirectory);
	const auto directoryIterator = m_impl->openedPack.directoryLookup.find(normalizedDirectory);

	if (directoryIterator == m_impl->openedPack.directoryLookup.end()) {
		throw std::runtime_error("The requested directory is not present in the pack.");
	}

	const auto& directory = m_impl->openedPack.directories.at(directoryIterator->second);
	DirectoryListingResult result;
	result.normalizedPath = directory.normalizedPath;
	result.children.reserve(static_cast<std::size_t>(directory.childDirectoryCount + directory.childFileCount));


	for (std::uint32_t index = 0U; index < directory.childDirectoryCount; ++index) {
		const auto& childDirectory = m_impl->openedPack.directories.at(directory.firstChildDirectoryIndex + index);
		DirectoryChild child;
		child.kind = PackNodeKind::Directory;
		child.normalizedPath = childDirectory.normalizedPath;
		child.name = io::ExtractLeafName(childDirectory.normalizedPath);
		result.children.push_back(std::move(child));
	}

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


ByteBuffer PackReader::ReadAll(std::string_view logicalPath) const {
	const auto normalizedPath = io::NormalizeLookupPath(logicalPath);
	const auto found = m_impl->openedPack.fileLookup.find(normalizedPath);

	if (found == m_impl->openedPack.fileLookup.end()) {
		throw std::runtime_error("The requested path is not present in the pack.");
	}

	const auto& entry = m_impl->openedPack.entries.at(found->second);
	return archive::ReadLogicalBytes(m_impl->openedPack, normalizedPath, 0U, entry.logicalSize, true);
}


ByteBuffer PackReader::ReadRange(std::string_view logicalPath, std::uint64_t offset, std::uint64_t size) const {
	return archive::ReadLogicalBytes(m_impl->openedPack, logicalPath, offset, size);
}


void PackReader::Extract(std::string_view logicalPath, const std::filesystem::path& outputDirectory, std::uint32_t workerCount) const {
	archive::ExtractRequest request;
	request.normalizedPath = io::NormalizeLookupPath(logicalPath);
	request.outputDirectory = outputDirectory;
	request.workerCount = workerCount;
	archive::ExtractLogicalPath(m_impl->openedPack, request);
}

} // namespace tepak
