// ExtentReader.cpp — TEPAK archive extent reading implementation
//
// Manages the process of reading and decompressing individual data extents from a TEPAK archive. It handles logical byte range mapping, worker pool coordination for parallel extraction, and dictionary-aware decompression via zstd.

#include "archive/ArchiveWorkflow.h"

#include "codec/CodecProvider.h"
#include "io/PathNormalizer.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {

// FindEntry is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const tepak::PackEntry& FindEntry(const tepak::archive::OpenedPack& openedPack, std::string_view logicalPath) {
	const auto normalizedPath = tepak::io::NormalizeLookupPath(logicalPath);
	const auto found = openedPack.fileLookup.find(normalizedPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (found == openedPack.fileLookup.end()) {
		throw std::runtime_error("The requested path is not present in the pack.");
	}
	return openedPack.entries.at(found->second);
}

// ResolveWorkerCount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint32_t ResolveWorkerCount(std::uint32_t requestedWorkers) {
	if (requestedWorkers == 0U) {
		return 1U;
	}

	const auto hardwareWorkers = std::thread::hardware_concurrency();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hardwareWorkers == 0U) {
		return requestedWorkers;
	}
	return std::min(requestedWorkers, hardwareWorkers);
}

// ReadStoredBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
tepak::ByteBuffer ReadStoredBytes(std::ifstream& stream, const tepak::PackExtent& extent) {
	tepak::ByteBuffer storedBytes(extent.storedSize);
	stream.seekg(static_cast<std::streamoff>(extent.storedOffset), std::ios::beg);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extent.storedSize > 0U) {
		stream.read(reinterpret_cast<char*>(storedBytes.data()), static_cast<std::streamsize>(extent.storedSize));
		if (!stream) {
			throw std::runtime_error("Failed to read stored extent bytes.");
		}
	}
	return storedBytes;
}

// ExtractEntryToPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ExtractEntryToPath(const tepak::archive::OpenedPack& openedPack, const tepak::PackEntry& entry, const std::filesystem::path& outputPath) {
	std::filesystem::create_directories(outputPath.parent_path());
	std::ifstream input(openedPack.packPath, std::ios::binary);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!input) {
		throw std::runtime_error("Failed to reopen the pack for extraction.");
	}
	std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!output) {
		throw std::runtime_error("Failed to open the extract destination.");
	}

	const tepak::codec::CodecProvider codecProvider;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t index = 0U; index < entry.entryExtentCount; ++index) {
		const auto& extent = openedPack.extents.at(entry.firstExtentIndex + index);
		const auto storedBytes = ReadStoredBytes(input, extent);
		const auto decodedBytes = codecProvider.Decompress(storedBytes,
			extent.codec,
			static_cast<std::size_t>(extent.uncompressedSize),
			openedPack.dictionaryBytes,
			&openedPack.preparedDictionary);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!decodedBytes.empty()) {
			output.write(reinterpret_cast<const char*>(decodedBytes.data()), static_cast<std::streamsize>(decodedBytes.size()));
			if (!output) {
				throw std::runtime_error("Failed to write extracted file bytes.");
			}
		}
	}
}

void CollectDirectoryEntries(const tepak::archive::OpenedPack& openedPack,
	std::uint32_t rootDirectoryId,
	std::vector<const tepak::PackEntry*>& outputEntries) {
	// TEPAK already records the direct child spans for every directory. Walking
	// those spans visits only the requested subtree, so extraction time grows
	// with the extracted content instead of every entry in the archive.
	std::vector<std::uint32_t> pendingDirectoryIds;
	pendingDirectoryIds.push_back(rootDirectoryId);
	while (!pendingDirectoryIds.empty()) {
		const auto directoryId = pendingDirectoryIds.back();
		pendingDirectoryIds.pop_back();

		const auto& directory = openedPack.directories.at(directoryId);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::uint32_t index = 0U; index < directory.childFileCount; ++index) {
			outputEntries.push_back(&openedPack.entries.at(directory.firstChildFileIndex + index));
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

// ReadLogicalBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ByteBuffer ReadLogicalBytes(const OpenedPack& openedPack, std::string_view logicalPath, std::uint64_t offset, std::uint64_t size) {
	const auto& entry = FindEntry(openedPack, logicalPath);
	if (offset > entry.logicalSize || (offset + size) > entry.logicalSize) {
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
	std::size_t outputOffset = 0U;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::uint32_t index = 0U; index < entry.entryExtentCount; ++index) {
		const auto& extent = openedPack.extents.at(entry.firstExtentIndex + index);
		const auto extentStart = extent.fileOffset;
		const auto extentEnd = extent.fileOffset + extent.uncompressedSize;
		const auto requestStart = offset;
		const auto requestEnd = offset + size;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (requestEnd <= extentStart || requestStart >= extentEnd) {
			continue;
		}

		const auto storedBytes = ReadStoredBytes(stream, extent);
		const auto decodedBytes = codecProvider.Decompress(storedBytes,
			extent.codec,
			static_cast<std::size_t>(extent.uncompressedSize),
			openedPack.dictionaryBytes,
			&openedPack.preparedDictionary);
		const auto copyStart = std::max<std::uint64_t>(requestStart, extentStart);
		const auto copyEnd = std::min<std::uint64_t>(requestEnd, extentEnd);
		const auto localOffset = copyStart - extentStart;
		const auto localSize = copyEnd - copyStart;
		std::copy_n(decodedBytes.begin() + static_cast<std::ptrdiff_t>(localOffset),
			static_cast<std::ptrdiff_t>(localSize),
			output.begin() + static_cast<std::ptrdiff_t>(outputOffset));
		outputOffset += static_cast<std::size_t>(localSize);
	}

	return output;
}

// ExtractLogicalPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ExtractLogicalPath(const OpenedPack& openedPack, const ExtractRequest& request) {
	const auto normalizedPath = io::NormalizeLookupPath(request.normalizedPath);
	const auto fileIterator = openedPack.fileLookup.find(normalizedPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (fileIterator != openedPack.fileLookup.end()) {
		ExtractEntryToPath(openedPack, openedPack.entries.at(fileIterator->second), request.outputDirectory / std::filesystem::path(normalizedPath));
		return;
	}

	const auto directoryIterator = openedPack.directoryLookup.find(normalizedPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (directoryIterator == openedPack.directoryLookup.end()) {
		throw std::runtime_error("The requested path is not present in the pack.");
	}

	std::vector<const PackEntry*> matchingEntries;
	CollectDirectoryEntries(openedPack, directoryIterator->second, matchingEntries);

	util::WorkerPool workerPool(ResolveWorkerCount(request.workerCount));
	std::vector<std::future<void>> futures;
	futures.reserve(matchingEntries.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto* entry : matchingEntries) {
		futures.push_back(workerPool.Submit([&openedPack, outputDirectory = request.outputDirectory, entry]() {
			ExtractEntryToPath(openedPack, *entry, outputDirectory / std::filesystem::path(entry->normalizedPath));
		}));
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& future : futures) {
		future.get();
	}
}

} // namespace tepak::archive
