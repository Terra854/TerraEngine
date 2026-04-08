#include "TEPAK/PackWriter.h"

#include "archive/ArchiveWorkflow.h"
#include "codec/CodecProvider.h"
#include "model/PackModel.h"
#include "util/BinaryIO.h"

#include "xxhash.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace {

std::vector<tepak::model::BinaryDirectoryRecord> BuildDirectoryRecords(const tepak::model::PackPlan& plan) {
	std::vector<tepak::model::BinaryDirectoryRecord> records;
	records.reserve(plan.directories.size());
	for (const auto& directory : plan.directories) {
		tepak::model::BinaryDirectoryRecord record;
		record.pathHash = directory.pathHash;
		record.pathOffset = directory.pathOffset;
		record.pathLength = directory.pathLength;
		record.parentDirectoryId = directory.parentDirectoryId;
		record.firstChildDirectoryIndex = directory.firstChildDirectoryIndex;
		record.childDirectoryCount = directory.childDirectoryCount;
		record.firstChildFileIndex = directory.firstChildFileIndex;
		record.childFileCount = directory.childFileCount;
		records.push_back(record);
	}
	return records;
}

std::vector<tepak::model::BinaryEntryRecord> BuildEntryRecords(const tepak::model::PackPlan& plan) {
	std::vector<tepak::model::BinaryEntryRecord> records;
	records.reserve(plan.entries.size());
	for (const auto& entry : plan.entries) {
		tepak::model::BinaryEntryRecord record;
		record.parentDirectoryId = entry.parentDirectoryId;
		record.pathHash = entry.pathHash;
		record.pathOffset = entry.pathOffset;
		record.pathLength = entry.pathLength;
		record.entryFlags = entry.entryFlags;
		record.assetClass = tepak::archive::EncodeAssetClass(entry.assetClass);
		record.firstExtentIndex = entry.firstExtentIndex;
		record.entryExtentCount = entry.entryExtentCount;
		record.logicalSize = entry.logicalSize;
		record.storedSize = entry.storedSize;
		record.entryHash = entry.entryHash;
		records.push_back(record);
	}
	return records;
}

std::vector<tepak::model::BinaryExtentRecord> BuildExtentRecords(const tepak::model::PackPlan& plan) {
	std::vector<tepak::model::BinaryExtentRecord> records;
	records.reserve(plan.extents.size());
	for (const auto& extent : plan.extents) {
		tepak::model::BinaryExtentRecord record;
		record.fileOffset = extent.fileOffset;
		record.storedOffset = extent.storedOffset;
		record.storedSize = extent.storedSize;
		record.uncompressedSize = extent.uncompressedSize;
		record.extentHash = extent.extentHash;
		record.codec = static_cast<std::uint32_t>(extent.codec);
		record.extentFlags = 0U;
		records.push_back(record);
	}
	return records;
}

tepak::ByteBuffer BuildMetadataBytes(const std::vector<tepak::model::BinaryDirectoryRecord>& directoryRecords,
	const std::vector<tepak::model::BinaryEntryRecord>& entryRecords,
	const std::vector<tepak::model::BinaryExtentRecord>& extentRecords,
	const std::string& pathTable) {
	// The metadata hash covers the exact serialized tables, so this helper builds
	// the same byte stream the writer later emits into the archive header region.
	tepak::ByteBuffer metadataBytes;
	metadataBytes.reserve(
		(directoryRecords.size() * sizeof(tepak::model::BinaryDirectoryRecord)) +
		(entryRecords.size() * sizeof(tepak::model::BinaryEntryRecord)) +
		(extentRecords.size() * sizeof(tepak::model::BinaryExtentRecord)) +
		pathTable.size());

	for (const auto& record : directoryRecords) {
		const auto* raw = reinterpret_cast<const std::uint8_t*>(&record);
		metadataBytes.insert(metadataBytes.end(), raw, raw + sizeof(record));
	}
	for (const auto& record : entryRecords) {
		const auto* raw = reinterpret_cast<const std::uint8_t*>(&record);
		metadataBytes.insert(metadataBytes.end(), raw, raw + sizeof(record));
	}
	for (const auto& record : extentRecords) {
		const auto* raw = reinterpret_cast<const std::uint8_t*>(&record);
		metadataBytes.insert(metadataBytes.end(), raw, raw + sizeof(record));
	}
	metadataBytes.insert(metadataBytes.end(), pathTable.begin(), pathTable.end());
	return metadataBytes;
}

tepak::model::CompressedExtentResult CompressExtentBytes(const tepak::codec::CodecProvider& codecProvider,
	tepak::ByteBuffer bytes,
	const tepak::ByteBuffer& dictionaryBytes,
	const tepak::codec::PreparedCompressionDictionary* preparedDictionary,
	bool enableCompression,
	std::int32_t compressionLevel) {
	tepak::model::CompressedExtentResult result;
	result.uncompressedSize = bytes.size();
	result.extentHash = codecProvider.HashBytes(bytes);
	if (!enableCompression) {
		result.mode = tepak::StorageMode::Raw;
		result.storedBytes = std::move(bytes);
		return result;
	}

	const auto decision = codecProvider.ChooseCompression(bytes, dictionaryBytes, enableCompression, compressionLevel, preparedDictionary);
	result.mode = decision.mode;
	result.storedBytes = decision.storedBytes;
	return result;
}

std::uint32_t MaxInFlightTasks(std::uint32_t workerCount) {
	return std::max<std::uint32_t>(2U, workerCount * 2U);
}

struct CommittedExtent final {
	std::uint32_t extentIndex = 0;
	std::uint32_t entryIndex = 0;
};

struct PackProgressState final {
	std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point lastReportTime = startTime;
	std::uint64_t totalLogicalBytes = 0;
	std::uint64_t processedLogicalBytes = 0;
	std::uint32_t totalEntries = 0;
	std::uint32_t completedEntries = 0;
	std::uint32_t totalExtents = 0;
	std::uint32_t completedExtents = 0;
	std::vector<std::uint32_t> remainingExtentsByEntry;
};

PackProgressState BuildPackProgressState(const tepak::model::PackPlan& plan, const tepak::model::PackagingInventory& inventory) {
	PackProgressState state;
	state.totalLogicalBytes = inventory.totalInputBytes;
	state.totalEntries = static_cast<std::uint32_t>(plan.entries.size());
	state.totalExtents = static_cast<std::uint32_t>(plan.extents.size());
	state.remainingExtentsByEntry.reserve(plan.entries.size());
	for (const auto& entry : plan.entries) {
		state.remainingExtentsByEntry.push_back(entry.entryExtentCount);
	}
	return state;
}

void MaybeEmitProgress(const tepak::PackagingRequest& request,
	PackProgressState& state,
	std::uint64_t archiveSizeSoFar,
	std::string_view currentEntryPath,
	bool force,
	bool finalUpdate) {
	if (!request.progressCallback) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (!force && request.progressUpdateIntervalMs > 0U) {
		const auto elapsedSinceLastReport = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastReportTime);
		if (elapsedSinceLastReport.count() < request.progressUpdateIntervalMs) {
			return;
		}
	}

	tepak::PackProgress progress;
	progress.totalLogicalBytes = state.totalLogicalBytes;
	progress.processedLogicalBytes = state.processedLogicalBytes;
	progress.archiveSizeSoFar = archiveSizeSoFar;
	progress.totalEntries = state.totalEntries;
	progress.completedEntries = state.completedEntries;
	progress.totalExtents = state.totalExtents;
	progress.completedExtents = state.completedExtents;
	progress.currentEntryPath = std::string(currentEntryPath);
	progress.elapsedSeconds = std::chrono::duration<double>(now - state.startTime).count();
	progress.finalUpdate = finalUpdate;

	if (progress.elapsedSeconds > 0.0 && state.processedLogicalBytes > 0U && state.processedLogicalBytes < state.totalLogicalBytes) {
		const auto remainingLogicalBytes = state.totalLogicalBytes - state.processedLogicalBytes;
		const auto logicalBytesPerSecond = static_cast<double>(state.processedLogicalBytes) / progress.elapsedSeconds;
		if (logicalBytesPerSecond > 0.0) {
			progress.estimatedRemainingSeconds = static_cast<double>(remainingLogicalBytes) / logicalBytesPerSecond;
			progress.etaAvailable = true;
		}
	}

	request.progressCallback(progress);
	state.lastReportTime = now;
}

void RecordCommittedExtentProgress(const tepak::PackagingRequest& request,
	const tepak::model::PackPlan& plan,
	PackProgressState& state,
	const CommittedExtent committedExtent,
	std::uint64_t archiveSizeSoFar,
	bool force,
	bool finalUpdate) {
	const auto& extent = plan.extents.at(committedExtent.extentIndex);
	const auto& entry = plan.entries.at(committedExtent.entryIndex);
	state.processedLogicalBytes += extent.uncompressedSize;
	++state.completedExtents;

	auto& remainingExtents = state.remainingExtentsByEntry.at(entry.fileId);
	if (remainingExtents > 0U) {
		--remainingExtents;
		if (remainingExtents == 0U) {
			++state.completedEntries;
		}
	}

	// Progress is reported after the extent has been committed to disk so the
	// archive size and completed-byte counts match the durable pack state.
	MaybeEmitProgress(request, state, archiveSizeSoFar, entry.normalizedPath, force, finalUpdate);
}

CommittedExtent CommitPendingFront(std::ofstream& stream,
	tepak::model::PackPlan& plan,
	std::vector<tepak::model::PendingCompressionTask>& pendingTasks,
	std::uint64_t& currentStoredOffset) {
	// Extents are compressed in parallel but committed in plan order so the on-
	// disk tables can describe one strictly forward-moving payload stream.
	auto pendingTask = std::move(pendingTasks.front());
	pendingTasks.erase(pendingTasks.begin());

	auto result = pendingTask.future.get();
	auto& extent = plan.extents.at(pendingTask.extentIndex);
	auto& entry = plan.entries.at(pendingTask.entryIndex);
	extent.codec = result.mode;
	extent.extentHash = result.extentHash;
	extent.uncompressedSize = result.uncompressedSize;
	extent.storedOffset = currentStoredOffset;
	extent.storedSize = result.storedBytes.size();
	entry.storedSize += extent.storedSize;
	plan.descriptor.codecMask |= (1ULL << static_cast<std::uint32_t>(extent.codec));

	if (!result.storedBytes.empty()) {
		stream.write(reinterpret_cast<const char*>(result.storedBytes.data()), static_cast<std::streamsize>(result.storedBytes.size()));
		if (!stream) {
			throw std::runtime_error("Failed to write compressed extent bytes.");
		}
	}
	currentStoredOffset += extent.storedSize;
	return {pendingTask.extentIndex, pendingTask.entryIndex};
}

} // namespace

namespace tepak {

PackWriteResult PackWriter::Create(const PackagingRequest& request) {
	if (std::filesystem::exists(request.outputPackPath) && !request.overwriteExisting) {
		throw std::runtime_error("Refusing to overwrite an existing pack without explicit permission.");
	}

	const codec::CodecProvider codecProvider;
	const auto inventory = archive::BuildPackagingInventory(request);
	auto plan = archive::BuildPackPlan(request, inventory, codecProvider);
	auto progressState = BuildPackProgressState(plan, inventory);
	const auto preparedDictionary = plan.dictionaryBytes.empty()
		? codec::PreparedCompressionDictionary {}
		: codecProvider.PrepareCompressionDictionary(plan.dictionaryBytes, request.compressionLevel);
	const auto dictionaryHash = codecProvider.HashBytes(plan.dictionaryBytes);

	model::BinaryHeader header;
	// The header layout is computed up front so the writer can reserve the full
	// metadata region and then stream payload bytes immediately afterward.
	header.headerSize = static_cast<std::uint32_t>(sizeof(model::BinaryHeader));
	header.flags = archive::EncodePackFlags(plan.dictionary.dictionaryPresent);
	header.directoryCount = static_cast<std::uint32_t>(plan.directories.size());
	header.fileCount = static_cast<std::uint32_t>(plan.entries.size());
	header.extentCount = static_cast<std::uint32_t>(plan.extents.size());
	header.defaultExtentSize = plan.descriptor.defaultExtentSize;
	header.rootDirectoryId = 0U;
	header.pathTableBytes = plan.pathTable.size();

	header.directoryTableOffset = util::AlignValue(header.headerSize, model::kMetadataAlignment);
	header.fileTableOffset = util::AlignValue(header.directoryTableOffset + (header.directoryCount * sizeof(model::BinaryDirectoryRecord)), model::kMetadataAlignment);
	header.extentTableOffset = util::AlignValue(header.fileTableOffset + (header.fileCount * sizeof(model::BinaryEntryRecord)), model::kMetadataAlignment);
	header.pathTableOffset = util::AlignValue(header.extentTableOffset + (header.extentCount * sizeof(model::BinaryExtentRecord)), model::kMetadataAlignment);
	header.dictionaryOffset = util::AlignValue(header.pathTableOffset + plan.pathTable.size(), model::kMetadataAlignment);
	header.dictionarySize = plan.dictionaryBytes.size();
	header.dataRegionOffset = util::AlignValue(header.dictionaryOffset + plan.dictionaryBytes.size(), model::kMetadataAlignment);
	header.dictionaryHash = dictionaryHash;

	if (!request.outputPackPath.parent_path().empty()) {
		std::filesystem::create_directories(request.outputPackPath.parent_path());
	}
	std::ofstream stream(request.outputPackPath, std::ios::binary | std::ios::trunc);
	if (!stream) {
		throw std::runtime_error("Failed to open the output pack for writing.");
	}

	// The writer reserves the metadata region once, then streams payload bytes
	// forward so compression never needs archive-sized in-memory staging buffers.
	util::WritePadding(stream, header.dataRegionOffset);

	util::WorkerPool workerPool(plan.workerCountUsed);
	std::vector<model::PendingCompressionTask> pendingTasks;
	pendingTasks.reserve(MaxInFlightTasks(plan.workerCountUsed));
	std::uint64_t currentStoredOffset = header.dataRegionOffset;

	for (std::uint32_t fileIndex = 0U; fileIndex < inventory.stagedFiles.size(); ++fileIndex) {
		const auto& stagedFile = inventory.stagedFiles[fileIndex];
		auto& entry = plan.entries.at(fileIndex);

		std::ifstream input(stagedFile.sourcePath, std::ios::binary);
		if (!input) {
			throw std::runtime_error("Failed to reopen staged input for streaming pack creation.");
		}

		XXH3_state_t hashState {};
		XXH3_64bits_reset(&hashState);
		for (std::uint32_t localExtentIndex = 0U; localExtentIndex < entry.entryExtentCount; ++localExtentIndex) {
			const auto extentIndex = entry.firstExtentIndex + localExtentIndex;
			const auto& extentPlan = plan.extents.at(extentIndex);
			ByteBuffer chunkBytes(static_cast<std::size_t>(extentPlan.uncompressedSize));
			if (extentPlan.uncompressedSize > 0U) {
				input.read(reinterpret_cast<char*>(chunkBytes.data()), static_cast<std::streamsize>(chunkBytes.size()));
				if (!input) {
					throw std::runtime_error("Failed to stream staged input bytes during pack creation.");
				}
				XXH3_64bits_update(&hashState, chunkBytes.data(), chunkBytes.size());
			}

			model::PendingCompressionTask pendingTask;
			pendingTask.extentIndex = extentIndex;
			pendingTask.entryIndex = fileIndex;
			// The plan stays alive until every pending future has been committed, so
			// each worker can share the selected dictionary bytes and the prepared
			// dictionary tables instead of rebuilding them for every extent.
			pendingTask.future = workerPool.Submit([&codecProvider,
				chunkBytes = std::move(chunkBytes),
				&dictionaryBytes = plan.dictionaryBytes,
				preparedDictionary,
				enableCompression = request.enableCompression,
				compressionLevel = request.compressionLevel]() mutable {
				return CompressExtentBytes(codecProvider, std::move(chunkBytes), dictionaryBytes, &preparedDictionary, enableCompression, compressionLevel);
			});
			pendingTasks.push_back(std::move(pendingTask));

			if (pendingTasks.size() >= MaxInFlightTasks(plan.workerCountUsed)) {
				const auto committedExtent = CommitPendingFront(stream, plan, pendingTasks, currentStoredOffset);
				RecordCommittedExtentProgress(request, plan, progressState, committedExtent, currentStoredOffset, false, false);
			}
		}

		entry.entryHash = XXH3_64bits_digest(&hashState);
	}

	while (!pendingTasks.empty()) {
		const auto committedExtent = CommitPendingFront(stream, plan, pendingTasks, currentStoredOffset);
		RecordCommittedExtentProgress(request, plan, progressState, committedExtent, currentStoredOffset, false, false);
	}
	MaybeEmitProgress(request,
		progressState,
		currentStoredOffset,
		plan.entries.empty() ? std::string_view{} : std::string_view(plan.entries.back().normalizedPath),
		true,
		true);

	header.archiveSize = currentStoredOffset;
	header.codecMask = plan.descriptor.codecMask;
	header.archiveIdentifier = plan.descriptor.archiveIdentifier;

	const auto directoryRecords = BuildDirectoryRecords(plan);
	const auto entryRecords = BuildEntryRecords(plan);
	const auto extentRecords = BuildExtentRecords(plan);
	const auto metadataBytes = BuildMetadataBytes(directoryRecords, entryRecords, extentRecords, plan.pathTable);
	header.metadataHash = codecProvider.HashBytes(metadataBytes);

	// Payload bytes are already on disk, so the second pass only back-fills the
	// metadata tables into the reserved header region at the front of the file.
	stream.seekp(0, std::ios::beg);
	util::WritePod(stream, header);
	util::WritePadding(stream, header.directoryTableOffset - header.headerSize);
	for (const auto& record : directoryRecords) {
		util::WritePod(stream, record);
	}
	util::WritePadding(stream, header.fileTableOffset - static_cast<std::uint64_t>(stream.tellp()));
	for (const auto& record : entryRecords) {
		util::WritePod(stream, record);
	}
	util::WritePadding(stream, header.extentTableOffset - static_cast<std::uint64_t>(stream.tellp()));
	for (const auto& record : extentRecords) {
		util::WritePod(stream, record);
	}
	util::WritePadding(stream, header.pathTableOffset - static_cast<std::uint64_t>(stream.tellp()));
	if (!plan.pathTable.empty()) {
		stream.write(plan.pathTable.data(), static_cast<std::streamsize>(plan.pathTable.size()));
	}
	util::WritePadding(stream, header.dictionaryOffset - static_cast<std::uint64_t>(stream.tellp()));
	if (!plan.dictionaryBytes.empty()) {
		stream.write(reinterpret_cast<const char*>(plan.dictionaryBytes.data()), static_cast<std::streamsize>(plan.dictionaryBytes.size()));
	}

	PackWriteResult result;
	result.descriptor.archiveIdentifier = header.archiveIdentifier;
	result.descriptor.headerSize = header.headerSize;
	result.descriptor.flags = header.flags;
	result.descriptor.directoryCount = header.directoryCount;
	result.descriptor.fileCount = header.fileCount;
	result.descriptor.extentCount = header.extentCount;
	result.descriptor.defaultExtentSize = header.defaultExtentSize;
	result.descriptor.rootDirectoryId = header.rootDirectoryId;
	result.descriptor.pathTableBytes = header.pathTableBytes;
	result.descriptor.directoryTableOffset = header.directoryTableOffset;
	result.descriptor.fileTableOffset = header.fileTableOffset;
	result.descriptor.extentTableOffset = header.extentTableOffset;
	result.descriptor.pathTableOffset = header.pathTableOffset;
	result.descriptor.dictionaryOffset = header.dictionaryOffset;
	result.descriptor.dictionarySize = header.dictionarySize;
	result.descriptor.dataRegionOffset = header.dataRegionOffset;
	result.descriptor.archiveSize = header.archiveSize;
	result.descriptor.dictionaryHash = header.dictionaryHash;
	result.descriptor.metadataHash = header.metadataHash;
	result.descriptor.codecMask = header.codecMask;
	result.descriptor.versionMajor = header.versionMajor;
	result.descriptor.versionMinor = header.versionMinor;
	result.dictionary = plan.dictionary;
	result.directories = plan.directories;
	result.entries = plan.entries;
	result.workerCountUsed = plan.workerCountUsed;
	result.estimatedPeakResidentBytes = plan.estimatedPeakResidentBytes;
	for (const auto& extentPlan : plan.extents) {
		PackExtent extent;
		extent.fileOffset = extentPlan.fileOffset;
		extent.storedOffset = extentPlan.storedOffset;
		extent.storedSize = extentPlan.storedSize;
		extent.uncompressedSize = extentPlan.uncompressedSize;
		extent.extentHash = extentPlan.extentHash;
		extent.codec = extentPlan.codec;
		result.extents.push_back(std::move(extent));
	}
	return result;
}

} // namespace tepak
