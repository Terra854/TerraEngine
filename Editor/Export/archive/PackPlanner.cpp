#include "archive/ArchiveWorkflow.h"

#include <algorithm>
#include <numeric>
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

} // namespace

namespace tepak::archive {

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

			if (sizeWithDictionary + 32U < sizeWithoutDictionary) {
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

		const auto extentCount = stagedFile.logicalSize == 0U
			? 1U
			: static_cast<std::uint32_t>((stagedFile.logicalSize + plan.descriptor.defaultExtentSize - 1U) / plan.descriptor.defaultExtentSize);
		entry.entryExtentCount = extentCount;

		// Every logical file is split into independently decodable extents so the
		// reader can service range reads without touching unrelated payload bytes.
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
	const auto dictionaryTrainingResidentBytes = dictionaryTrainingBufferBytes == 0U
		? sampleResidentBytes
		: (sampleResidentBytes + sampleResidentBytes + dictionaryTrainingBufferBytes);
	const auto compressionResidentBytes = sampleResidentBytes + metadataResidentBytes + plan.dictionaryBytes.size() + residentExtentBytes + compressionContextResidentBytes + preparedCompressionDictionaryResidentBytes;
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
