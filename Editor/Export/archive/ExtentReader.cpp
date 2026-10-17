// ExtentReader.cpp — TEPAK archive extent reading implementation
//
// Manages the process of reading and decompressing individual data extents from a TEPAK archive. It handles logical byte range mapping, worker pool coordination for parallel extraction, and dictionary-aware decompression via zstd.

#include "archive/ArchiveWorkflow.h"

#include "codec/CodecProvider.h"
#include "io/PathNormalizer.h"
#include "io/ExtractionDestination.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {


const tepak::PackEntry& FindEntry(const tepak::archive::OpenedPack& openedPack, std::string_view logicalPath) {
	const auto normalizedPath = tepak::io::NormalizeLookupPath(logicalPath);
	const auto found = openedPack.fileLookup.find(normalizedPath);

	if (found == openedPack.fileLookup.end()) {
		throw std::runtime_error("The requested path is not present in the pack.");
	}
	return openedPack.entries.at(found->second);
}


std::uint32_t ResolveWorkerCount(std::uint32_t requestedWorkers) {
	if (requestedWorkers == 0U) {
		return 1U;
	}

	const auto hardwareWorkers = std::thread::hardware_concurrency();

	if (hardwareWorkers == 0U) {
		return requestedWorkers;
	}
	return std::min(requestedWorkers, hardwareWorkers);
}


tepak::ByteBuffer ReadStoredBytes(std::ifstream& stream, const tepak::PackExtent& extent) {
	tepak::ByteBuffer storedBytes(extent.storedSize);
	stream.seekg(static_cast<std::streamoff>(extent.storedOffset), std::ios::beg);

	if (extent.storedSize > 0U) {
		stream.read(reinterpret_cast<char*>(storedBytes.data()), static_cast<std::streamsize>(extent.storedSize));
		if (!stream) {
			throw std::runtime_error("Failed to read stored extent bytes.");
		}
	}
	return storedBytes;
}


void ExtractEntryToPath(const tepak::archive::OpenedPack& openedPack, const tepak::PackEntry& entry, const tepak::io::ExtractionDestination& destination) {
	// Complete verification precedes any destination publication. A rejected
	// payload cannot truncate an earlier successful extraction.
	const auto decoded=tepak::archive::ReadLogicalBytes(openedPack,entry.normalizedPath,0,entry.logicalSize,true);
	destination.Publish(entry.normalizedPath,decoded);
}

void CollectDirectoryEntries(const tepak::archive::OpenedPack& openedPack,
	std::uint32_t rootDirectoryId,
	std::vector<const tepak::PackEntry*>& outputEntries) {
	// TEPAK already records the direct child spans for every directory. Walking
	// those spans visits only the requested subtree, so extraction time grows
	// with the extracted content instead of every entry in the archive.
	std::vector<bool> visitedDirectories(openedPack.directories.size()), visitedFiles(openedPack.entries.size());
	std::vector<std::uint32_t> pendingDirectoryIds;
	pendingDirectoryIds.push_back(rootDirectoryId);
	while (!pendingDirectoryIds.empty()) {
		const auto directoryId = pendingDirectoryIds.back();
		pendingDirectoryIds.pop_back();

		if(directoryId >= visitedDirectories.size() || visitedDirectories[directoryId]) throw std::runtime_error("Archive subtree revisits a directory.");
		visitedDirectories[directoryId]=true;
		const auto& directory = openedPack.directories.at(directoryId);
		if(directory.firstChildDirectoryIndex>visitedDirectories.size() || directory.childDirectoryCount>visitedDirectories.size()-directory.firstChildDirectoryIndex || directory.firstChildFileIndex>visitedFiles.size() || directory.childFileCount>visitedFiles.size()-directory.firstChildFileIndex) throw std::runtime_error("Archive subtree has invalid child spans.");

		for (std::uint32_t index = 0U; index < directory.childFileCount; ++index) {
			const auto fileId=directory.firstChildFileIndex+index; if(visitedFiles[fileId]) throw std::runtime_error("Archive subtree revisits a file."); visitedFiles[fileId]=true;
			outputEntries.push_back(&openedPack.entries.at(fileId));
		}

		// Reverse push keeps sibling directories in their stored order when the
		// stack is popped, which matches the metadata layout used elsewhere.
		for (std::uint32_t index = directory.childDirectoryCount; index > 0U; --index) {
			pendingDirectoryIds.push_back(directory.firstChildDirectoryIndex + (index - 1U));
		}
	}
}

} // namespace

namespace tepak::archive {


ByteBuffer ReadLogicalBytes(const OpenedPack& openedPack, std::string_view logicalPath, std::uint64_t offset, std::uint64_t size, bool verifyEntry) {
	const auto& entry = FindEntry(openedPack, logicalPath);
	if (offset > entry.logicalSize || size > entry.logicalSize - offset) {
		throw std::runtime_error("The requested byte range is outside the logical file size.");
	}

	// Range reads decode only the extents that overlap the requested byte span.
	// That keeps random access practical for a mounted archive.
	ByteBuffer output(size);
	std::ifstream stream(openedPack.packPath, std::ios::binary);
	if (!stream) {
		throw std::runtime_error("Failed to reopen the pack for readback.");
	}

	const codec::CodecProvider codecProvider;
	// Destination offsets come from logical overlap, never concatenation order.

	for (std::uint32_t index = 0U; index < entry.entryExtentCount; ++index) {
		const auto& extent = openedPack.extents.at(entry.firstExtentIndex + index);
		const auto extentStart = extent.fileOffset;
		const auto extentEnd = extent.fileOffset + extent.uncompressedSize;
		const auto requestStart = offset;
		const auto requestEnd = offset + size;

		if (!(verifyEntry && entry.logicalSize == 0) && (requestEnd <= extentStart || requestStart >= extentEnd)) {
			continue;
		}

		const auto storedBytes = ReadStoredBytes(stream, extent);
		const auto decodedBytes = codecProvider.Decompress(storedBytes,
			extent.codec,
			static_cast<std::size_t>(extent.uncompressedSize),
			openedPack.dictionaryBytes,
			&openedPack.preparedDictionary);
		if(codecProvider.HashBytes(decodedBytes)!=extent.extentHash) throw std::runtime_error("Decoded archive extent hash mismatch.");
		const auto copyStart = std::max<std::uint64_t>(requestStart, extentStart);
		const auto copyEnd = std::min<std::uint64_t>(requestEnd, extentEnd);
		const auto localOffset = copyStart - extentStart;
		const auto localSize = copyEnd - copyStart;
		std::copy_n(decodedBytes.begin() + static_cast<std::ptrdiff_t>(localOffset),
			static_cast<std::ptrdiff_t>(localSize),
			output.begin() + static_cast<std::ptrdiff_t>(copyStart - offset));
	}

	// A full logical result verifies the entry hash; empty range-only calls
	// remain distinct from full empty-file validation.
	if((verifyEntry || (offset==0 && size==entry.logicalSize && size!=0)) && codecProvider.HashBytes(output)!=entry.entryHash) throw std::runtime_error("Decoded archive entry hash mismatch.");

	return output;
}


void ExtractLogicalPath(const OpenedPack& openedPack, const ExtractRequest& request) {
	const auto normalizedPath=io::NormalizeLookupPath(request.normalizedPath);
	std::vector<const PackEntry*> matchingEntries;
	if(const auto file=openedPack.fileLookup.find(normalizedPath); file!=openedPack.fileLookup.end()) matchingEntries.push_back(&openedPack.entries.at(file->second));
	else {
		const auto directory=openedPack.directoryLookup.find(normalizedPath);
		if(directory==openedPack.directoryLookup.end()) throw std::runtime_error("The requested path is not present in the pack.");
		CollectDirectoryEntries(openedPack,directory->second,matchingEntries);
	}
	std::vector<std::string> selected; selected.reserve(matchingEntries.size());
	for(const auto* entry:matchingEntries) selected.push_back(entry->normalizedPath);
	// Preflight and anchors span the entire job cohort, including failure drain.
	io::ExtractionDestination destination(request.outputDirectory,selected);
	util::WorkerPool workerPool(ResolveWorkerCount(request.workerCount));
	std::vector<std::future<void>> futures; futures.reserve(matchingEntries.size());
	for(const auto* entry:matchingEntries) futures.push_back(workerPool.Submit([&openedPack,&destination,entry] { ExtractEntryToPath(openedPack,*entry,destination); }));
	for(auto& future:futures) future.get();
}

} // namespace tepak::archive
