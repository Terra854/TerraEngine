// Writer-generated archive corpus shared by both native reader harnesses.
// Mutations touch persisted bytes independently of reader validation.
#pragma once
#include "TEPAK/PackWriter.h"
#include "model/PackModel.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Utilities/UUID.h"
#include "xxhash.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Tests::ArchiveSafety {
	using Bytes = tepak::ByteBuffer;
	enum class Variant { Raw, Compressed, Dictionary, Paged, Empty, EmptyTree };
	inline std::uint64_t Hash(std::span<const std::uint8_t> bytes) { return XXH3_64bits(bytes.data(), bytes.size()); }
	inline Bytes ReadFile(const std::filesystem::path& path) {
		std::ifstream input(path, std::ios::binary);
		if(!input) throw std::runtime_error("Archive fixture input could not be opened.");
		Bytes bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		if(input.bad()) throw std::runtime_error("Archive fixture input could not be read.");
		return bytes;
	}
	inline void WriteFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		output.close(); if(!output) throw std::runtime_error("Archive fixture output could not be written.");
	}
	// Test corruption must itself use checked physical slices, even after a
	// deliberately impossible count/offset is placed into an archive header.
	class Image {
	public:
		Bytes bytes;
		explicit Image(Bytes value) : bytes(std::move(value)) {}
		explicit Image(const std::filesystem::path& path) : bytes(ReadFile(path)) {}
		std::span<const std::uint8_t> Slice(std::uint64_t offset, std::uint64_t length) const {
			if(offset > bytes.size() || length > bytes.size() - offset) throw std::runtime_error("Archive fixture slice exceeds physical bytes.");
			return std::span<const std::uint8_t>(bytes).subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
		}
		template<class T> T Read(std::uint64_t offset) const {
			static_assert(std::is_trivially_copyable_v<T>); T value;
			const auto source = Slice(offset, sizeof(T)); std::memcpy(&value, source.data(), sizeof(T)); return value;
		}
		template<class T> void Write(std::uint64_t offset, const T& value) {
			static_assert(std::is_trivially_copyable_v<T>); (void)Slice(offset, sizeof(T));
			std::memcpy(bytes.data() + static_cast<std::size_t>(offset), &value, sizeof(T));
		}
		tepak::model::BinaryHeader Header() const { return Read<tepak::model::BinaryHeader>(0); }
		void SetHeader(const tepak::model::BinaryHeader& value) { Write(0, value); }
		std::uint64_t RecordOffset(std::uint64_t start, std::uint64_t count, std::uint64_t index, std::uint64_t stride) const {
			if(stride == 0 || index >= count || start > bytes.size() || count > (bytes.size() - start) / stride)
				throw std::runtime_error("Archive fixture table exceeds physical bytes.");
			return start + index * stride;
		}
		tepak::model::BinaryDirectoryRecord Directory(std::uint32_t index) const {
			const auto h = Header(); return Read<tepak::model::BinaryDirectoryRecord>(RecordOffset(h.directoryTableOffset, h.directoryCount, index, sizeof(tepak::model::BinaryDirectoryRecord)));
		}
		tepak::model::BinaryEntryRecord Entry(std::uint32_t index) const {
			const auto h = Header(); return Read<tepak::model::BinaryEntryRecord>(RecordOffset(h.fileTableOffset, h.fileCount, index, sizeof(tepak::model::BinaryEntryRecord)));
		}
		tepak::model::BinaryExtentRecord Extent(std::uint32_t index) const {
			const auto h = Header(); return Read<tepak::model::BinaryExtentRecord>(RecordOffset(h.extentTableOffset, h.extentCount, index, sizeof(tepak::model::BinaryExtentRecord)));
		}
		void SetDirectory(std::uint32_t index, const tepak::model::BinaryDirectoryRecord& value) {
			const auto h = Header(); Write(RecordOffset(h.directoryTableOffset, h.directoryCount, index, sizeof(value)), value);
		}
		void SetEntry(std::uint32_t index, const tepak::model::BinaryEntryRecord& value) {
			const auto h = Header(); Write(RecordOffset(h.fileTableOffset, h.fileCount, index, sizeof(value)), value);
		}
		void SetExtent(std::uint32_t index, const tepak::model::BinaryExtentRecord& value) {
			const auto h = Header(); Write(RecordOffset(h.extentTableOffset, h.extentCount, index, sizeof(value)), value);
		}
		std::string Path(std::uint32_t offset, std::uint32_t length) const {
			const auto h = Header();
			if(offset > h.pathTableBytes || length > h.pathTableBytes - offset) throw std::runtime_error("Archive fixture path exceeds its table.");
			const auto part = Slice(h.pathTableOffset, h.pathTableBytes).subspan(offset, length); return {part.begin(), part.end()};
		}
		std::uint32_t FindEntry(std::string_view path) const {
			for(std::uint32_t index = 0; index < Header().fileCount; ++index) { const auto e = Entry(index); if(Path(e.pathOffset, e.pathLength) == path) return index; }
			throw std::runtime_error("Authored archive fixture entry is missing.");
		}
		void SetStoredPath(std::uint32_t index, std::string_view path) {
			// Use an existing sufficiently long writer slot; section addresses and
			// unrelated payloads stay unchanged when testing unsafe path names.
			auto e = Entry(index); const auto h = Header();
			if(path.size() > e.pathLength) throw std::runtime_error("Archive fixture path exceeds reserved slot.");
			(void)Path(e.pathOffset, e.pathLength);
			std::memcpy(bytes.data() + static_cast<std::size_t>(h.pathTableOffset + e.pathOffset), path.data(), path.size());
			e.pathLength = static_cast<std::uint32_t>(path.size()); e.pathHash = XXH3_64bits(path.data(), path.size()); SetEntry(index, e);
		}
		void Flip(std::uint64_t offset) { (void)Slice(offset, 1); bytes[static_cast<std::size_t>(offset)] ^= 0x5aU; }
		void CorruptExtentPayload(std::uint32_t index) {
			// Poison an unrelated stored extent to prove that a range read does
			// not decode or validate payloads outside the requested logical span.
			const auto e = Extent(index);
			if(e.storedSize == 0) throw std::runtime_error("Empty archive fixture extent has no payload byte to corrupt.");
			(void)Slice(e.storedOffset, e.storedSize); Flip(e.storedOffset);
		}
		void RehashMetadata() {
			auto h = Header(); Bytes metadata;
			const auto append = [&](std::uint64_t offset, std::uint64_t count, std::uint64_t stride) {
				if(offset > bytes.size() || stride == 0 || count > (bytes.size() - offset) / stride) throw std::runtime_error("Archive fixture rehash exceeds physical bytes.");
				const auto part = Slice(offset, count * stride); metadata.insert(metadata.end(), part.begin(), part.end());
			};
			append(h.directoryTableOffset, h.directoryCount, sizeof(tepak::model::BinaryDirectoryRecord));
			append(h.fileTableOffset, h.fileCount, sizeof(tepak::model::BinaryEntryRecord));
			append(h.extentTableOffset, h.extentCount, sizeof(tepak::model::BinaryExtentRecord));
			const auto paths = Slice(h.pathTableOffset, h.pathTableBytes); metadata.insert(metadata.end(), paths.begin(), paths.end());
			h.metadataHash = Hash(metadata); SetHeader(h);
		}
		void Save(const std::filesystem::path& path) const { WriteFile(path, bytes); }
	};
	inline Bytes Pattern(std::size_t length, std::uint32_t seed = 0x71c93a65U) {
		Bytes bytes(length); for(auto& b : bytes) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; b = static_cast<std::uint8_t>(seed); } return bytes;
	}
	inline Image DeepTree(std::uint32_t count) {
		// Emit the writer's persisted records directly: no platform path-length
		// limit or source-directory privilege can mask a tree parser failure.
		if(count == 0) throw std::runtime_error("Deep archive fixture needs a root.");
		tepak::model::BinaryHeader h; h.headerSize = sizeof(h); h.directoryCount = count; h.fileCount = 1; h.extentCount = 1;
		h.defaultExtentSize = 4096; h.codecMask = 1; h.directoryTableOffset = sizeof(h);
		h.fileTableOffset = h.directoryTableOffset + count * sizeof(tepak::model::BinaryDirectoryRecord);
		h.extentTableOffset = h.fileTableOffset + sizeof(tepak::model::BinaryEntryRecord);
		h.pathTableOffset = h.extentTableOffset + sizeof(tepak::model::BinaryExtentRecord);
		std::vector<tepak::model::BinaryDirectoryRecord> directories(count); Bytes paths; std::string path;
		for(std::uint32_t index = 0; index < count; ++index) {
			auto& d = directories[index]; d.parentDirectoryId = index == 0 ? tepak::kInvalidDirectoryId : index - 1;
			d.pathOffset = static_cast<std::uint32_t>(paths.size()); d.pathLength = static_cast<std::uint32_t>(path.size()); d.pathHash = XXH3_64bits(path.data(), path.size());
			paths.insert(paths.end(), path.begin(), path.end()); d.firstChildDirectoryIndex = index + 1; d.childDirectoryCount = index + 1 < count ? 1 : 0;
			d.firstChildFileIndex = index == 0 ? 0 : 1; d.childFileCount = index == 0 ? 1 : 0;
			path += index == 0 ? "a" : "/a";
		}
		tepak::model::BinaryEntryRecord entry; entry.parentDirectoryId = 0; entry.pathOffset = static_cast<std::uint32_t>(paths.size()); entry.pathLength = 8; entry.pathHash = XXH3_64bits("root.bin",8); entry.entryExtentCount = 1; entry.entryHash = Hash({});
		paths.insert(paths.end(), {'r','o','o','t','.','b','i','n'}); h.pathTableBytes = paths.size(); h.dictionaryOffset = (h.pathTableOffset + h.pathTableBytes + 7) & ~std::uint64_t{7};
		h.dataRegionOffset = h.dictionaryOffset; h.archiveSize = h.dataRegionOffset; h.dictionaryHash = Hash({});
		Image image(Bytes(static_cast<std::size_t>(h.archiveSize))); image.SetHeader(h);
		for(std::uint32_t index = 0; index < count; ++index) image.SetDirectory(index, directories[index]);
		image.SetEntry(0,entry); tepak::model::BinaryExtentRecord extent; extent.storedOffset = h.dataRegionOffset; extent.extentHash = Hash({}); image.SetExtent(0,extent);
		std::copy(paths.begin(), paths.end(), image.bytes.begin() + static_cast<std::size_t>(h.pathTableOffset)); image.RehashMetadata(); return image;
	}
	inline Bytes PagedTemf() {
		// Two declared canonical page ranges exercise writer extent planning.
		// Geometry interpretation is outside these byte-identical archive tests.
		constexpr std::uint64_t first = 40, second = 56;
		const std::uint64_t pageOffset = sizeof(TemfProductionHeader), blockOffset = pageOffset + 2 * sizeof(TemfPageDescriptor);
		const auto payloadOffset = blockOffset + 2 * sizeof(TemfBlockDescriptor);
		Bytes bytes(static_cast<std::size_t>(payloadOffset + first + second));
		TemfProductionHeader h{}; h.logicalPageSizeBytes = kTemfPageSize64KiB; h.pageCount = 2; h.blockCount = 2;
		h.pageTableOffset = pageOffset; h.blockTableOffset = blockOffset; h.payloadOffset = payloadOffset; h.payloadBytes = first + second; h.directoryBytes = payloadOffset - sizeof(h);
		std::memcpy(bytes.data(), &h, sizeof(h));
		std::array<TemfPageDescriptor, 2> pages{}; std::array<TemfBlockDescriptor, 2> blocks{};
		for(std::uint32_t index = 0; index < 2; ++index) {
			pages[index].logicalPageId = index + 3; pages[index].firstBlockIndex = index; pages[index].blockCount = 1; pages[index].attributedStoredBytes = index == 0 ? first : second;
			blocks[index].logicalPageId = pages[index].logicalPageId; blocks[index].encodedPayloadOffset = payloadOffset + (index == 0 ? 0 : first); blocks[index].encodedBytes = pages[index].attributedStoredBytes;
		}
		std::memcpy(bytes.data() + pageOffset, pages.data(), sizeof(pages)); std::memcpy(bytes.data() + blockOffset, blocks.data(), sizeof(blocks));
		const auto payload = Pattern(first + second); std::copy(payload.begin(), payload.end(), bytes.begin() + static_cast<std::size_t>(payloadOffset)); return bytes;
	}
	class Fixture {
	public:
		Fixture() : root(std::filesystem::temp_directory_path() / "TerraEngineArchiveSafety" / TerraEngine::ToHexString(TerraEngine::GenerateUUID())) { std::filesystem::create_directories(SourceRoot()); }
		~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
		Fixture(const Fixture&) = delete; Fixture& operator=(const Fixture&) = delete;
		std::filesystem::path SourceRoot() const { return root / "source"; }
		std::filesystem::path PackPath() const { return root / "archive.tepak"; }
		std::filesystem::path ExtractionRoot() const { return root / "extracted"; }
		std::filesystem::path OutsideRoot() const { return root / "outside"; }
		std::filesystem::path MutationPath(std::string_view name = "mutated.tepak") const { return root / name; }
		std::map<std::string, Bytes> authored;
		void WriteSource(std::string path, Bytes bytes) { WriteFile(SourceRoot() / path, bytes); authored.insert_or_assign(std::move(path), std::move(bytes)); }
		static bool HasCodec(const tepak::PackWriteResult& result, tepak::StorageMode codec) {
			return std::any_of(result.extents.begin(), result.extents.end(), [codec](const auto& e) { return e.codec == codec; });
		}
		tepak::PackWriteResult Build(Variant variant = Variant::Raw) {
			if(variant == Variant::EmptyTree) {
				// The writer refuses a source set without files. Derive its root-only
				// representation from a valid empty file, retaining serialized layout.
				auto result = Build(Variant::Empty); Image image(PackPath());
				auto rootRecord = image.Directory(0); rootRecord.firstChildFileIndex = 0; rootRecord.childFileCount = 0; image.SetDirectory(0, rootRecord);
				auto h = image.Header(); h.fileCount = 0; h.extentCount = 0; h.pathTableBytes = 0; image.SetHeader(h);
				image.RehashMetadata(); image.Save(PackPath()); authored.clear();
				result.entries.clear(); result.extents.clear(); result.directories[0].childFileCount = 0;
				result.descriptor.fileCount = 0; result.descriptor.extentCount = 0; result.descriptor.pathTableBytes = 0;
				return result;
			}
			// Reset only this owned source tree; no live mounts or engine globals.
			std::filesystem::remove_all(SourceRoot()); std::filesystem::create_directories(SourceRoot()); authored.clear();
			if(variant == Variant::Dictionary) {
				const auto shared = Pattern(3072);
				for(std::uint32_t index = 0; index < 64; ++index) { auto content = shared; const auto tail = Pattern(1024, index + 17); content.insert(content.end(), tail.begin(), tail.end()); WriteSource("training/sample-" + std::to_string(index) + ".json", std::move(content)); }
			} else if(variant != Variant::EmptyTree) {
				WriteSource("empty.bin", {});
				if(variant != Variant::Empty) {
					WriteSource("nested/safe-content.bin", variant == Variant::Compressed ? Bytes(12288, 'A') : Pattern(12288));
					WriteSource("branch/other.bin", Pattern(8192, 53));
					if(variant == Variant::Paged) WriteSource("meshes/paged.temf", PagedTemf());
				}
			}
			auto result = BuildSources(variant == Variant::Compressed || variant == Variant::Dictionary, variant == Variant::Dictionary);
			if(variant == Variant::Dictionary && (!result.dictionary.dictionaryPresent || !HasCodec(result, tepak::StorageMode::ZstdDictionary))) throw std::runtime_error("Archive fixture corpus did not exercise dictionary storage.");
			if(variant == Variant::Compressed && !HasCodec(result, tepak::StorageMode::Zstd)) throw std::runtime_error("Archive fixture corpus did not exercise zstd storage.");
			return result;
		}
		tepak::PackWriteResult BuildSources(bool compressed = false, bool dictionary = false) const {
			// Custom deep-tree/path fixtures retain their authored source corpus.
			tepak::PackagingRequest request;
			request.inputRoot = SourceRoot(); request.outputPackPath = PackPath(); request.extentSizingBytes = 4096;
			request.workerCount = 1; request.overwriteExisting = true; request.enableCompression = compressed;
			request.enableDictionaryTraining = dictionary; request.maxDictionaryBytes = 8192; request.peakMemoryBudgetBytes = 64ULL * 1024 * 1024;
			return tepak::PackWriter::Create(request);
		}
		Bytes OutsideSentinel(std::string_view name = "sentinel.bin") const { const auto bytes = Pattern(64, 127); WriteFile(OutsideRoot() / name, bytes); return bytes; }
	private:
		std::filesystem::path root;
	};
}
