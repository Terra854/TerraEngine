// CodecProvider.cpp — TEPAK compression codec provider implementation
//
// Implements the compression and decompression logic using the zstd library. It manages thread-local worker contexts, handles dictionary training from file samples, and provides memory usage estimates for various pack configurations.

#include "codec/CodecProvider.h"

#include "xxhash.h"

#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif

#include <zdict.h>
#include <zstd.h>

#include <algorithm>
#include <stdexcept>

namespace {

// ThrowIfZstdError is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ThrowIfZstdError(std::size_t code, const char* message) {
	if (ZSTD_isError(code) != 0U) {
		throw std::runtime_error(std::string(message) + ": " + ZSTD_getErrorName(code));
	}
}

// ResolveEstimatedSize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::size_t ResolveEstimatedSize(std::size_t code, const char* message) {
	ThrowIfZstdError(code, message);
	return code;
}

// ClampCompressionLevel is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
int ClampCompressionLevel(std::int32_t requestedLevel) {
	return std::clamp(requestedLevel, ZSTD_minCLevel(), ZSTD_maxCLevel());
}

// struct ThreadCodecContexts groups related state and operations so callers have one documented contract.
struct ThreadCodecContexts final {
	// TEPAK reuses one compression and decompression context per worker thread
	// so repeated extent operations do not pay full context creation cost.
	ZSTD_CCtx* compressionContext = ZSTD_createCCtx();
	ZSTD_DCtx* decompressionContext = ZSTD_createDCtx();

	~ThreadCodecContexts() {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (compressionContext != nullptr) {
			ZSTD_freeCCtx(compressionContext);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (decompressionContext != nullptr) {
			ZSTD_freeDCtx(decompressionContext);
		}
	}
};

// GetThreadCodecContexts is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ThreadCodecContexts& GetThreadCodecContexts() {
	thread_local ThreadCodecContexts contexts;
	if (contexts.compressionContext == nullptr || contexts.decompressionContext == nullptr) {
		throw std::runtime_error("Failed to create zstd worker contexts.");
	}
	return contexts;
}

} // namespace

namespace tepak::codec {

// struct CodecProvider groups related state and operations so callers have one documented contract.
struct CodecProvider::Impl final {
};

// struct PreparedCompressionDictionary groups related state and operations so callers have one documented contract.
struct PreparedCompressionDictionary::Impl final {
	ZSTD_CDict* dictionary = nullptr;

	~Impl() {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (dictionary != nullptr) {
			ZSTD_freeCDict(dictionary);
		}
	}
};

// struct PreparedDecompressionDictionary groups related state and operations so callers have one documented contract.
struct PreparedDecompressionDictionary::Impl final {
	ZSTD_DDict* dictionary = nullptr;

	~Impl() {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (dictionary != nullptr) {
			ZSTD_freeDDict(dictionary);
		}
	}
};

PreparedCompressionDictionary::PreparedCompressionDictionary() = default;
PreparedCompressionDictionary::~PreparedCompressionDictionary() = default;
PreparedCompressionDictionary::PreparedCompressionDictionary(const PreparedCompressionDictionary&) noexcept = default;
PreparedCompressionDictionary& PreparedCompressionDictionary::operator=(const PreparedCompressionDictionary&) noexcept = default;
PreparedCompressionDictionary::PreparedCompressionDictionary(PreparedCompressionDictionary&&) noexcept = default;
PreparedCompressionDictionary& PreparedCompressionDictionary::operator=(PreparedCompressionDictionary&&) noexcept = default;

// PreparedCompressionDictionary::IsAvailable is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool PreparedCompressionDictionary::IsAvailable() const {
	return m_impl != nullptr && m_impl->dictionary != nullptr;
}

PreparedDecompressionDictionary::PreparedDecompressionDictionary() = default;
PreparedDecompressionDictionary::~PreparedDecompressionDictionary() = default;
PreparedDecompressionDictionary::PreparedDecompressionDictionary(const PreparedDecompressionDictionary&) noexcept = default;
PreparedDecompressionDictionary& PreparedDecompressionDictionary::operator=(const PreparedDecompressionDictionary&) noexcept = default;
PreparedDecompressionDictionary::PreparedDecompressionDictionary(PreparedDecompressionDictionary&&) noexcept = default;
PreparedDecompressionDictionary& PreparedDecompressionDictionary::operator=(PreparedDecompressionDictionary&&) noexcept = default;

// PreparedDecompressionDictionary::IsAvailable is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool PreparedDecompressionDictionary::IsAvailable() const {
	return m_impl != nullptr && m_impl->dictionary != nullptr;
}

CodecProvider::CodecProvider() : m_impl(std::make_unique<Impl>()) {
}

CodecProvider::~CodecProvider() = default;
CodecProvider::CodecProvider(CodecProvider&&) noexcept = default;
CodecProvider& CodecProvider::operator=(CodecProvider&&) noexcept = default;

// CodecProvider::HashBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint64_t CodecProvider::HashBytes(const ByteBuffer& bytes) const {
	return XXH3_64bits(bytes.data(), bytes.size());
}

// CodecProvider::HashBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint64_t CodecProvider::HashBytes(const std::string& text) const {
	return XXH3_64bits(text.data(), text.size());
}

// CodecProvider::TrainDictionary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
DictionaryProfile CodecProvider::TrainDictionary(const std::vector<ByteBuffer>& samples, std::uint32_t maxDictionaryBytes) const {
	DictionaryProfile profile;
	const auto dictionaryBytes = TrainDictionaryBytes(samples, maxDictionaryBytes);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (dictionaryBytes.empty()) {
		return profile;
	}
	profile.dictionaryPresent = true;
	profile.dictionarySize = dictionaryBytes.size();
	profile.dictionaryHash = HashBytes(dictionaryBytes);
	profile.selectionMode = "trained";
	return profile;
}

// CodecProvider::TrainDictionaryBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ByteBuffer CodecProvider::TrainDictionaryBytes(const std::vector<ByteBuffer>& samples, std::uint32_t maxDictionaryBytes) const {
	if (samples.size() < 8U || maxDictionaryBytes == 0U) {
		return {};
	}

	std::size_t totalBytes = 0U;
	std::vector<std::size_t> sampleSizes;
	sampleSizes.reserve(samples.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& sample : samples) {
		totalBytes += sample.size();
		sampleSizes.push_back(sample.size());
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (totalBytes < (16U * 1024U)) {
		return {};
	}

	// zstd dictionary training consumes one contiguous training buffer plus a
	// side array of sample sizes, so the planner separately accounts for this
	// temporary peak before approving a pack configuration.
	const auto dictionaryCapacity = std::max<std::size_t>(4096U, std::min<std::size_t>(maxDictionaryBytes, totalBytes / 2U));
	ByteBuffer trainingData;
	trainingData.reserve(totalBytes);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& sample : samples) {
		trainingData.insert(trainingData.end(), sample.begin(), sample.end());
	}

	ByteBuffer dictionaryBytes(dictionaryCapacity);
	const auto trainedSize = ZDICT_trainFromBuffer(dictionaryBytes.data(), dictionaryBytes.size(), trainingData.data(), sampleSizes.data(), sampleSizes.size());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ZDICT_isError(trainedSize) != 0U || trainedSize < 1024U) {
		return {};
	}

	dictionaryBytes.resize(trainedSize);
	return dictionaryBytes;
}

// CodecProvider::EstimateCompressionContextBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint64_t CodecProvider::EstimateCompressionContextBytes(std::size_t sourceBytes, std::int32_t compressionLevel, std::size_t dictionaryBytes) const {
	// The pack planner uses the same one-shot compression path as the worker
	// threads, so the estimate needs to scale with both extent size and
	// compression level instead of assuming every worker costs the same amount.
	const auto zstdCompressionLevel = ClampCompressionLevel(compressionLevel);
	const auto compressionParameters = ZSTD_getCParams(zstdCompressionLevel,
		static_cast<unsigned long long>(sourceBytes),
		dictionaryBytes);
	return static_cast<std::uint64_t>(ResolveEstimatedSize(
		ZSTD_estimateCCtxSize_usingCParams(compressionParameters),
		"Failed to estimate zstd compression context size"));
}

// CodecProvider::EstimatePreparedCompressionDictionaryBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint64_t CodecProvider::EstimatePreparedCompressionDictionaryBytes(std::size_t sourceBytes, std::int32_t compressionLevel, std::size_t dictionaryBytes) const {
	if (dictionaryBytes < 8U) {
		return 0U;
	}

	// Prepared dictionaries are separate resident allocations from the worker
	// compression contexts, so they need their own memory-budget term.
	const auto zstdCompressionLevel = ClampCompressionLevel(compressionLevel);
	const auto compressionParameters = ZSTD_getCParams(zstdCompressionLevel,
		static_cast<unsigned long long>(sourceBytes),
		dictionaryBytes);
	return static_cast<std::uint64_t>(ResolveEstimatedSize(
		ZSTD_estimateCDictSize_advanced(dictionaryBytes, compressionParameters, ZSTD_dlm_byRef),
		"Failed to estimate zstd prepared dictionary size"));
}

// CodecProvider::PrepareCompressionDictionary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
PreparedCompressionDictionary CodecProvider::PrepareCompressionDictionary(const ByteBuffer& dictionaryBytes, std::int32_t compressionLevel) const {
	PreparedCompressionDictionary preparedDictionary;
	if (dictionaryBytes.size() < 8U) {
		return preparedDictionary;
	}

	auto state = std::make_shared<PreparedCompressionDictionary::Impl>();
	// TEPAK keeps the dictionary byte buffer alive for the whole pack build, so
	// the prepared dictionary can safely reference that buffer instead of copying it.
	state->dictionary = ZSTD_createCDict_byReference(dictionaryBytes.data(),
		dictionaryBytes.size(),
		ClampCompressionLevel(compressionLevel));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (state->dictionary == nullptr) {
		throw std::runtime_error("Failed to prepare the pack compression dictionary.");
	}

	preparedDictionary.m_impl = std::move(state);
	return preparedDictionary;
}

// CodecProvider::PrepareDecompressionDictionary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
PreparedDecompressionDictionary CodecProvider::PrepareDecompressionDictionary(const ByteBuffer& dictionaryBytes) const {
	PreparedDecompressionDictionary preparedDictionary;
	if (dictionaryBytes.size() < 8U) {
		return preparedDictionary;
	}

	auto state = std::make_shared<PreparedDecompressionDictionary::Impl>();
	// Opened packs own their dictionary bytes, so the prepared decompression
	// dictionary can point at that stable storage for all future reads.
	state->dictionary = ZSTD_createDDict_byReference(dictionaryBytes.data(), dictionaryBytes.size());
	if (state->dictionary == nullptr) {
		throw std::runtime_error("Failed to prepare the pack decompression dictionary.");
	}

	preparedDictionary.m_impl = std::move(state);
	return preparedDictionary;
}

CompressionDecision CodecProvider::ChooseCompression(const ByteBuffer& bytes,
	const ByteBuffer& dictionaryBytes,
	bool allowCompression,
	std::int32_t compressionLevel,
	const PreparedCompressionDictionary* preparedDictionary) const {
	CompressionDecision bestDecision;
	bestDecision.mode = StorageMode::Raw;
	bestDecision.storedBytes = bytes;

	// Very small payloads usually lose more to frame overhead than they gain
	// from compression, so the raw path is the correct default for them.
	if (!allowCompression || bytes.size() < 64U) {
		return bestDecision;
	}

	// Clamp to the linked zstd library range so callers can tune aggressively
	// without having to duplicate codec-specific validation at higher layers.
	const auto zstdCompressionLevel = ClampCompressionLevel(compressionLevel);

	const auto tryCompress = [&](StorageMode mode, const ByteBuffer& dictionary) {
		CompressionDecision decision;
		decision.mode = mode;
		decision.storedBytes.resize(ZSTD_compressBound(bytes.size()));

		std::size_t compressedSize = 0U;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (dictionary.empty()) {
			auto& contexts = GetThreadCodecContexts();
			compressedSize = ZSTD_compressCCtx(contexts.compressionContext,
				decision.storedBytes.data(),
				decision.storedBytes.size(),
				bytes.data(),
				bytes.size(),
				zstdCompressionLevel);
		} else if (preparedDictionary != nullptr && preparedDictionary->IsAvailable()) {
			auto& contexts = GetThreadCodecContexts();
			// The prepared dictionary path avoids reparsing the same pack-level
			// dictionary bytes for every extent in dictionary-enabled archives.
			compressedSize = ZSTD_compress_usingCDict(contexts.compressionContext,
				decision.storedBytes.data(),
				decision.storedBytes.size(),
				bytes.data(),
				bytes.size(),
				preparedDictionary->m_impl->dictionary);
		} else {
			auto& contexts = GetThreadCodecContexts();
			compressedSize = ZSTD_compress_usingDict(contexts.compressionContext,
				decision.storedBytes.data(),
				decision.storedBytes.size(),
				bytes.data(),
				bytes.size(),
				dictionary.data(),
				dictionary.size(),
				zstdCompressionLevel);
		}

		ThrowIfZstdError(compressedSize, "Failed to compress extent bytes");
		decision.storedBytes.resize(compressedSize);
		decision.score = static_cast<double>(bytes.size()) - static_cast<double>(compressedSize);
		return decision;
	};

	const auto plainDecision = tryCompress(StorageMode::Zstd, {});
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (plainDecision.storedBytes.size() + 8U < bestDecision.storedBytes.size()) {
		bestDecision = plainDecision;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!dictionaryBytes.empty()) {
		const auto dictionaryDecision = tryCompress(StorageMode::ZstdDictionary, dictionaryBytes);
		if (dictionaryDecision.storedBytes.size() + 8U < bestDecision.storedBytes.size()) {
			bestDecision = dictionaryDecision;
		}
	}

	return bestDecision;
}

ByteBuffer CodecProvider::Decompress(const ByteBuffer& storedBytes,
	StorageMode mode,
	std::size_t expectedSize,
	const ByteBuffer& dictionaryBytes,
	const PreparedDecompressionDictionary* preparedDictionary) const {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mode == StorageMode::Raw) {
		// Raw decoding is identity only when the declared length matches exactly.
		if(storedBytes.size() != expectedSize) throw std::runtime_error("Raw extent size mismatch.");
		return storedBytes;
	}

	if(mode != StorageMode::Zstd && mode != StorageMode::ZstdDictionary) throw std::runtime_error("Unknown extent codec.");
	if(mode == StorageMode::ZstdDictionary && dictionaryBytes.empty()) throw std::runtime_error("Missing extent dictionary.");

	// Every extent is decoded independently. That keeps random access simple and
	// avoids hidden cross-extent history like a solid archive would require.
	// Frame headers expose a cheap length check before decoded allocation.
	// Unknown content sizes remain supported and are checked after decoding.
	const auto frameSize=ZSTD_getFrameContentSize(storedBytes.data(),storedBytes.size());
	if(frameSize==ZSTD_CONTENTSIZE_ERROR || (frameSize!=ZSTD_CONTENTSIZE_UNKNOWN && frameSize!=expectedSize)) throw std::runtime_error("Compressed frame length disagrees with extent metadata.");
	ByteBuffer output(expectedSize);
	std::size_t decompressedSize = 0U;
	if (mode == StorageMode::Zstd) {
		auto& contexts = GetThreadCodecContexts();
		decompressedSize = ZSTD_decompressDCtx(contexts.decompressionContext,
			output.data(),
			output.size(),
			storedBytes.data(),
			storedBytes.size());
	} else if (preparedDictionary != nullptr && preparedDictionary->IsAvailable()) {
		auto& contexts = GetThreadCodecContexts();
		decompressedSize = ZSTD_decompress_usingDDict(contexts.decompressionContext,
			output.data(),
			output.size(),
			storedBytes.data(),
			storedBytes.size(),
			preparedDictionary->m_impl->dictionary);
	} else {
		auto& contexts = GetThreadCodecContexts();
		decompressedSize = ZSTD_decompress_usingDict(contexts.decompressionContext,
			output.data(),
			output.size(),
			storedBytes.data(),
			storedBytes.size(),
			dictionaryBytes.data(),
			dictionaryBytes.size());
	}

	ThrowIfZstdError(decompressedSize, "Failed to decompress extent bytes");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (decompressedSize != expectedSize) {
		throw std::runtime_error("Decoded extent size did not match the expected size.");
	}
	return output;
}

} // namespace tepak::codec
