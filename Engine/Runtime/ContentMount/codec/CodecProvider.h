// CodecProvider.h — Contains declarations for the Codec Provider file-specific types, helper functions, and module integration code used by runtime decompression, dictionary handling, and content hash verification helpers.

#pragma once

#include "TEPAK/PackTypes.h"

#include <memory>

namespace tepak::codec {

// CompressionDecision returns the chosen codec and the payload bytes that
// should actually be written for one extent.
struct CompressionDecision final {
	StorageMode mode = StorageMode::Raw;
	ByteBuffer storedBytes;
	double score = 0.0;
};

// PreparedCompressionDictionary wraps zstd's digested dictionary tables so the
// same pack dictionary can be reused across many extents cheaply.
class PreparedCompressionDictionary final {
public:
	PreparedCompressionDictionary();
	~PreparedCompressionDictionary();

	PreparedCompressionDictionary(const PreparedCompressionDictionary&) noexcept;
	PreparedCompressionDictionary& operator=(const PreparedCompressionDictionary&) noexcept;
	PreparedCompressionDictionary(PreparedCompressionDictionary&&) noexcept;
	PreparedCompressionDictionary& operator=(PreparedCompressionDictionary&&) noexcept;

	bool IsAvailable() const;

private:
	friend class CodecProvider;

	struct Impl;
	std::shared_ptr<const Impl> m_impl;
};

// PreparedDecompressionDictionary is the read-side equivalent cached by an
// opened pack for extract, validate, and random read operations.
class PreparedDecompressionDictionary final {
public:
	PreparedDecompressionDictionary();
	~PreparedDecompressionDictionary();

	PreparedDecompressionDictionary(const PreparedDecompressionDictionary&) noexcept;
	PreparedDecompressionDictionary& operator=(const PreparedDecompressionDictionary&) noexcept;
	PreparedDecompressionDictionary(PreparedDecompressionDictionary&&) noexcept;
	PreparedDecompressionDictionary& operator=(PreparedDecompressionDictionary&&) noexcept;

	bool IsAvailable() const;

private:
	friend class CodecProvider;

	struct Impl;
	std::shared_ptr<const Impl> m_impl;
};

// CodecProvider centralizes hashing, dictionary training, and compression
// policy so pack planning and readback use the exact same rules.
class CodecProvider final {
public:
	CodecProvider();
	~CodecProvider();

	CodecProvider(CodecProvider&&) noexcept;
	CodecProvider& operator=(CodecProvider&&) noexcept;

	CodecProvider(const CodecProvider&) = delete;
	CodecProvider& operator=(const CodecProvider&) = delete;

	// Content hashes are used for entry and extent integrity checks.
	std::uint64_t HashBytes(const ByteBuffer& bytes) const;
	std::uint64_t HashBytes(const std::string& text) const;
	// Builds dictionary metadata and the actual dictionary bytes from bounded
	// training samples collected during inventory discovery.
	DictionaryProfile TrainDictionary(const std::vector<ByteBuffer>& samples, std::uint32_t maxDictionaryBytes) const;
	ByteBuffer TrainDictionaryBytes(const std::vector<ByteBuffer>& samples, std::uint32_t maxDictionaryBytes) const;
	// Memory estimates let the planner reject impossible pack configurations
	// before the writer starts streaming payloads.
	std::uint64_t EstimateCompressionContextBytes(std::size_t sourceBytes, std::int32_t compressionLevel, std::size_t dictionaryBytes) const;
	std::uint64_t EstimatePreparedCompressionDictionaryBytes(std::size_t sourceBytes, std::int32_t compressionLevel, std::size_t dictionaryBytes) const;
	// Prepared dictionaries pay the setup cost once and are then shared by many
	// extent operations.
	PreparedCompressionDictionary PrepareCompressionDictionary(const ByteBuffer& dictionaryBytes, std::int32_t compressionLevel) const;
	PreparedDecompressionDictionary PrepareDecompressionDictionary(const ByteBuffer& dictionaryBytes) const;
	// Chooses between raw, plain zstd, and dictionary zstd for one extent.
	CompressionDecision ChooseCompression(const ByteBuffer& bytes,
		const ByteBuffer& dictionaryBytes,
		bool allowCompression,
		std::int32_t compressionLevel,
		const PreparedCompressionDictionary* preparedDictionary = nullptr) const;
	// Restores one extent to its logical bytes using the recorded storage mode.
	ByteBuffer Decompress(const ByteBuffer& storedBytes,
		StorageMode mode,
		std::size_t expectedSize,
		const ByteBuffer& dictionaryBytes,
		const PreparedDecompressionDictionary* preparedDictionary = nullptr) const;

private:

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace tepak::codec
