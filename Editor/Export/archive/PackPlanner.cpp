// PackPlanner.cpp — TEPAK archive pack planning implementation
//
// Implements the logic for converting a discovered file inventory into a deterministic TEPAK archive plan. It handles dictionary training evaluation, memory budget estimation, and the layout of metadata tables for optimized runtime access.

#include "archive/ArchiveWorkflow.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {


std::uint32_t ResolveWorkerCount(std::uint32_t requestedWorkers) {
	// Clamp to hardware concurrency so the standalone tool does not create more
	// compression workers than the host can actually run at once.

	if (requestedWorkers == 0U) {
		return 1U;
	}

	const auto hardwareWorkers = std::thread::hardware_concurrency();

	if (hardwareWorkers == 0U) {
		return requestedWorkers;
	}
	return std::min(requestedWorkers, hardwareWorkers);
}


void AppendPathText(std::string& pathTable, std::string_view text, std::uint32_t& offset, std::uint32_t& length) {
	// Directory and file records point into one shared string table instead of
	// each storing their own heap allocation in the on-disk metadata format.
	offset = static_cast<std::uint32_t>(pathTable.size());
	length = static_cast<std::uint32_t>(text.size());
	pathTable.append(text);
}


std::uint64_t SumSampleBytes(const std::vector<tepak::ByteBuffer>& samples) {
	return std::accumulate(samples.begin(), samples.end(), 0ULL, [](std::uint64_t total, const tepak::ByteBuffer& sample) {
		return total + static_cast<std::uint64_t>(sample.size());
	});
}


std::uint32_t ResolveMaxInFlightTasks(std::uint32_t workerCount) {
	// A little extra queue depth keeps workers fed while the main thread waits
	// for earlier extents to become ready for ordered archive writes.
	return std::max<std::uint32_t>(2U, workerCount * 2U);
}


std::uint64_t ResolveDictionaryTrainingBufferBytes(std::uint64_t totalSampleBytes, std::size_t sampleCount, std::uint32_t maxDictionaryBytes) {
	if (sampleCount < 8U || maxDictionaryBytes == 0U || totalSampleBytes < (16U * 1024U)) {
		return 0U;
	}

	return std::max<std::uint64_t>(4096U, std::min<std::uint64_t>(maxDictionaryBytes, totalSampleBytes / 2U));
}

// TryBuildTemfExtentSizes returns one metadata extent followed by one extent
// per canonical TEMF page. TEPAK already supports arbitrary contiguous extent
// sizes, so matching those boundaries avoids decoding an unrelated 1 MiB
// archive extent for a 32/64/128 KiB geometry request.
std::optional<std::vector<std::uint64_t>> TryBuildTemfExtentSizes(
	const tepak::model::StagedFile& stagedFile) {
	std::string extension = stagedFile.sourcePath.extension().generic_string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	if(extension != ".temf" || stagedFile.logicalSize < sizeof(TemfProductionHeader)) {
		return std::nullopt;
	}

	std::ifstream stream(stagedFile.sourcePath, std::ios::binary);
	if(!stream) {
		return std::nullopt;
	}
	const auto rangeFits = [fileSize = stagedFile.logicalSize](std::uint64_t offset,
		std::uint64_t count, std::uint64_t stride) {
		return offset <= fileSize && stride != 0U
			&& count <= (fileSize - offset) / stride;
	};
	const auto readAt = [&stream, &rangeFits](std::uint64_t offset, auto& value) {
		if(!rangeFits(offset, 1U, sizeof(value))) return false;
		stream.clear();
		stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		stream.read(reinterpret_cast<char*>(&value), sizeof(value));
		return static_cast<bool>(stream);
	};

	TemfProductionHeader header{};
	if(!readAt(0U, header)
		|| header.magic != kMeshMagicValue
		|| header.formatContractId != kTemfProductionContractId
		|| header.headerBytes != kTemfProductionHeaderBytes
		|| header.logicalPageSizeBytes == 0U
		|| header.pageCount == 0U || header.blockCount == 0U
		|| header.pageCount > kTemfMaximumPageCount
		|| header.blockCount > kTemfMaximumBlockCount
		|| !rangeFits(header.pageTableOffset, header.pageCount, sizeof(TemfPageDescriptor))
		|| !rangeFits(header.blockTableOffset, header.blockCount, sizeof(TemfBlockDescriptor))
		|| header.payloadOffset > stagedFile.logicalSize
		|| header.payloadBytes != stagedFile.logicalSize - header.payloadOffset) {
		return std::nullopt;
	}

	std::vector<TemfPageDescriptor> pages(header.pageCount);
	std::vector<TemfBlockDescriptor> blocks(header.blockCount);
	stream.clear();
	stream.seekg(static_cast<std::streamoff>(header.pageTableOffset), std::ios::beg);
	stream.read(reinterpret_cast<char*>(pages.data()),
		static_cast<std::streamsize>(pages.size() * sizeof(TemfPageDescriptor)));
	stream.clear();
	stream.seekg(static_cast<std::streamoff>(header.blockTableOffset), std::ios::beg);
	stream.read(reinterpret_cast<char*>(blocks.data()),
		static_cast<std::streamsize>(blocks.size() * sizeof(TemfBlockDescriptor)));
	if(!stream) return std::nullopt;

	struct PageRange final { std::uint64_t begin = 0U; std::uint64_t end = 0U; };
	std::vector<PageRange> ranges;
	ranges.reserve(pages.size());
	for(const TemfPageDescriptor& page : pages) {
		if(page.blockCount == 0U || page.firstBlockIndex > blocks.size()
			|| page.blockCount > blocks.size() - page.firstBlockIndex) {
			return std::nullopt;
		}
		std::uint64_t pageBytes = 0U;
		std::uint64_t expectedOffset = 0U;
		for(std::uint32_t ordinal = 0U; ordinal < page.blockCount; ++ordinal) {
			const TemfBlockDescriptor& block = blocks[page.firstBlockIndex + ordinal];
			if(block.logicalPageId != page.logicalPageId || block.blockOrdinal != ordinal
				|| block.encodedBytes == 0U
				|| block.encodedPayloadOffset > stagedFile.logicalSize
				|| block.encodedBytes > stagedFile.logicalSize - block.encodedPayloadOffset
				|| (ordinal != 0U && block.encodedPayloadOffset != expectedOffset)
				|| block.encodedBytes > std::numeric_limits<std::uint64_t>::max() - pageBytes) {
				return std::nullopt;
			}
			if(ordinal == 0U) expectedOffset = block.encodedPayloadOffset;
			pageBytes += block.encodedBytes;
			expectedOffset = block.encodedPayloadOffset + block.encodedBytes;
		}
		if(pageBytes != page.attributedStoredBytes) return std::nullopt;
		const TemfBlockDescriptor& first = blocks[page.firstBlockIndex];
		ranges.push_back({first.encodedPayloadOffset, expectedOffset});
	}
	std::sort(ranges.begin(), ranges.end(), [](const PageRange& left, const PageRange& right) {
		return left.begin < right.begin;
	});
	if(ranges.empty() || ranges.front().begin != header.payloadOffset) return std::nullopt;

	std::vector<std::uint64_t> extentSizes;
	extentSizes.reserve(ranges.size() + 1U);
	extentSizes.push_back(header.payloadOffset);
	std::uint64_t cursor = header.payloadOffset;
	for(const PageRange& range : ranges) {
		if(range.begin != cursor || range.end <= range.begin) return std::nullopt;
		extentSizes.push_back(range.end - range.begin);
		cursor = range.end;
	}
	if(cursor != stagedFile.logicalSize) return std::nullopt;
	return extentSizes;
}

} // namespace

namespace tepak::archive {

bool IsDictionaryStorageBeneficial(
	std::uint64_t bytesWithoutDictionary,
	std::uint64_t bytesWithDictionary,
	std::uint64_t dictionaryBytes) {
	constexpr std::uint64_t kSelectionMarginBytes = 32U;
	if(bytesWithDictionary > std::numeric_limits<std::uint64_t>::max() - dictionaryBytes
		|| bytesWithDictionary + dictionaryBytes
			> std::numeric_limits<std::uint64_t>::max() - kSelectionMarginBytes) {
		return false;
	}
	return bytesWithDictionary + dictionaryBytes + kSelectionMarginBytes
		< bytesWithoutDictionary;
}


model::PackPlan BuildPackPlan(const PackagingRequest& request, const model::PackagingInventory& inventory, const codec::CodecProvider& codecProvider) {
	model::PackPlan plan;
	plan.workerCountUsed = ResolveWorkerCount(request.workerCount);
	plan.descriptor.versionMajor = model::kVersionMajor;
	plan.descriptor.versionMinor = model::kVersionMinor;
	plan.descriptor.headerSize = static_cast<std::uint32_t>(sizeof(model::BinaryHeader));
	plan.descriptor.defaultExtentSize = std::max(4096U, request.extentSizingBytes);
	plan.descriptor.rootDirectoryId = 0U;

	// Dictionary selection stays data-driven, but the planner only keeps a
	// dictionary when held-out samples show a real byte win.
	if (request.enableCompression && request.enableDictionaryTraining && !inventory.dictionarySamples.empty()) {
		auto candidateDictionary = codecProvider.TrainDictionaryBytes(inventory.dictionarySamples, request.maxDictionaryBytes);
		if (!candidateDictionary.empty()) {
			const auto candidatePreparedDictionary = codecProvider.PrepareCompressionDictionary(candidateDictionary, request.compressionLevel);
			std::size_t sizeWithoutDictionary = 0U;
			std::size_t sizeWithDictionary = 0U;

			for (const auto& sample : inventory.dictionarySamples) {
				sizeWithoutDictionary += codecProvider.ChooseCompression(sample, {}, true, request.compressionLevel).storedBytes.size();
				sizeWithDictionary += codecProvider.ChooseCompression(sample,
					candidateDictionary,
					true,
					request.compressionLevel,
					&candidatePreparedDictionary).storedBytes.size();
			}


			if (IsDictionaryStorageBeneficial(
				sizeWithoutDictionary, sizeWithDictionary, candidateDictionary.size())) {
				// Reuse the accepted candidate bytes directly so the planner does
				// not pay for a second full zstd dictionary training pass.
				plan.dictionaryBytes = std::move(candidateDictionary);
				plan.dictionary.dictionaryPresent = true;
				plan.dictionary.dictionarySize = plan.dictionaryBytes.size();
				plan.dictionary.dictionaryHash = codecProvider.HashBytes(plan.dictionaryBytes);
				plan.dictionary.selectionMode = "trained";
				plan.dictionary.selectionScore = static_cast<double>(sizeWithoutDictionary - sizeWithDictionary);
			}
		}
	}

	plan.directories = inventory.directories;

	for (auto& directory : plan.directories) {
		directory.pathHash = codecProvider.HashBytes(directory.normalizedPath);
		AppendPathText(plan.pathTable, directory.normalizedPath, directory.pathOffset, directory.pathLength);
	}

	plan.entries.reserve(inventory.stagedFiles.size());
	plan.extents.reserve(inventory.stagedFiles.size());

	for (std::uint32_t fileId = 0U; fileId < inventory.stagedFiles.size(); ++fileId) {
		const auto& stagedFile = inventory.stagedFiles[fileId];
		PackEntry entry;
		entry.fileId = fileId;
		entry.parentDirectoryId = stagedFile.parentDirectoryId;
		entry.normalizedPath = stagedFile.normalizedPath;
		entry.name = stagedFile.name;
		entry.pathHash = codecProvider.HashBytes(stagedFile.normalizedPath);
		entry.assetClass = stagedFile.assetClass;
		entry.logicalSize = stagedFile.logicalSize;
		entry.firstExtentIndex = static_cast<std::uint32_t>(plan.extents.size());
		AppendPathText(plan.pathTable, entry.normalizedPath, entry.pathOffset, entry.pathLength);

		const std::optional<std::vector<std::uint64_t>> temfExtentSizes =
			TryBuildTemfExtentSizes(stagedFile);
		if(temfExtentSizes.has_value()) {
			if(temfExtentSizes->size() > std::numeric_limits<std::uint32_t>::max()) {
				throw std::runtime_error("A TEMF page layout exceeds TEPAK's extent-count limit.");
			}
			entry.entryExtentCount = static_cast<std::uint32_t>(temfExtentSizes->size());
			std::uint64_t fileOffset = 0U;
			for(const std::uint64_t extentBytes : *temfExtentSizes) {
				model::ExtentPlan extent;
				extent.entryIndex = fileId;
				extent.fileOffset = fileOffset;
				extent.uncompressedSize = extentBytes;
				plan.extents.push_back(std::move(extent));
				fileOffset += extentBytes;
			}
		} else {
			const auto extentCount = stagedFile.logicalSize == 0U
				? 1U
				: static_cast<std::uint32_t>((stagedFile.logicalSize + plan.descriptor.defaultExtentSize - 1U) / plan.descriptor.defaultExtentSize);
			entry.entryExtentCount = extentCount;

			// Non-paged files retain the general fixed-size policy. Only canonical
			// TEMF page boundaries need the specialized random-access layout.
			for (std::uint32_t localExtentIndex = 0U; localExtentIndex < extentCount; ++localExtentIndex) {
				model::ExtentPlan extent;
				extent.entryIndex = fileId;
				extent.fileOffset = static_cast<std::uint64_t>(localExtentIndex) * static_cast<std::uint64_t>(plan.descriptor.defaultExtentSize);
				const auto remainingBytes = stagedFile.logicalSize > extent.fileOffset ? stagedFile.logicalSize - extent.fileOffset : 0U;
				extent.uncompressedSize = stagedFile.logicalSize == 0U
					? 0U
					: std::min<std::uint64_t>(remainingBytes, plan.descriptor.defaultExtentSize);
				plan.extents.push_back(std::move(extent));
			}
		}

		plan.entries.push_back(std::move(entry));
	}

	plan.descriptor.directoryCount = static_cast<std::uint32_t>(plan.directories.size());
	plan.descriptor.fileCount = static_cast<std::uint32_t>(plan.entries.size());
	plan.descriptor.extentCount = static_cast<std::uint32_t>(plan.extents.size());
	plan.descriptor.pathTableBytes = static_cast<std::uint64_t>(plan.pathTable.size());

	for (const auto& extent : plan.extents) {
		plan.descriptor.codecMask |= (1ULL << static_cast<std::uint32_t>(extent.codec));
	}
	plan.descriptor.archiveIdentifier = codecProvider.HashBytes(plan.pathTable);
	plan.dictionary.compressionEligibleExtentCount = request.enableCompression ? static_cast<std::uint32_t>(plan.extents.size()) : 0U;

	const auto sampleResidentBytes = SumSampleBytes(inventory.dictionarySamples);
	const auto dictionaryTrainingBufferBytes = ResolveDictionaryTrainingBufferBytes(sampleResidentBytes, inventory.dictionarySamples.size(), request.maxDictionaryBytes);
	const auto metadataResidentBytes =
		static_cast<std::uint64_t>(plan.pathTable.size()) +
		(static_cast<std::uint64_t>(plan.directories.size()) * sizeof(model::BinaryDirectoryRecord)) +
		(static_cast<std::uint64_t>(plan.entries.size()) * sizeof(model::BinaryEntryRecord)) +
		(static_cast<std::uint64_t>(plan.extents.size()) * sizeof(model::BinaryExtentRecord));
	const auto maxInFlightExtents = ResolveMaxInFlightTasks(plan.workerCountUsed);
	const auto residentBytesPerExtent = request.enableCompression
		? (static_cast<std::uint64_t>(plan.descriptor.defaultExtentSize) * 2ULL + 4096ULL)
		: (static_cast<std::uint64_t>(plan.descriptor.defaultExtentSize) + 4096ULL);
	const auto residentExtentBytes = static_cast<std::uint64_t>(maxInFlightExtents) * residentBytesPerExtent;
	const auto compressionContextResidentBytes = request.enableCompression
		? (static_cast<std::uint64_t>(plan.workerCountUsed) * codecProvider.EstimateCompressionContextBytes(
			static_cast<std::size_t>(plan.descriptor.defaultExtentSize),
			request.compressionLevel,
			plan.dictionaryBytes.size()))
		: 0U;
	const auto preparedCompressionDictionaryResidentBytes = request.enableCompression
		? codecProvider.EstimatePreparedCompressionDictionaryBytes(
			static_cast<std::size_t>(plan.descriptor.defaultExtentSize),
			request.compressionLevel,
			plan.dictionaryBytes.size())
		: 0U;
	const std::uint64_t dictionaryTrainingResidentBytes = dictionaryTrainingBufferBytes == 0U
		? sampleResidentBytes
		: (sampleResidentBytes + sampleResidentBytes + dictionaryTrainingBufferBytes);
	const std::uint64_t compressionResidentBytes =
		sampleResidentBytes +
		metadataResidentBytes +
		static_cast<std::uint64_t>(plan.dictionaryBytes.size()) +
		residentExtentBytes +
		compressionContextResidentBytes +
		preparedCompressionDictionaryResidentBytes;
	// Peak memory can happen either while dictionary training builds its
	// contiguous sample buffer, or later while the worker pool holds in-flight
	// extent buffers alongside one reusable zstd context per worker thread.
	plan.estimatedPeakResidentBytes = std::max(dictionaryTrainingResidentBytes, compressionResidentBytes);
	if (plan.estimatedPeakResidentBytes > request.peakMemoryBudgetBytes) {
		throw std::runtime_error("The requested memory budget is too small for the configured extent size and worker count.");
	}

	return plan;
}

} // namespace tepak::archive
