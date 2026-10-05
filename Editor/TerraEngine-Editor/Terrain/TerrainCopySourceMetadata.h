// Bounded durable metadata capture shared by the editor owner and regressions.
#pragma once
#include "Terrain/TerrainTypes.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace TerraEngine::Editor::Terrain {
	// Capture small descriptor/pointer files only. Complete terrain blobs remain
	// worker preparation; the extra-byte check also rejects growth during capture.
	inline std::string ReadTerrainCopyMetadata(const std::filesystem::path& path, std::size_t maximumBytes) {
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		const auto length = input ? input.tellg() : std::streampos{-1};
		if (length < 0 || static_cast<std::uint64_t>(length) > maximumBytes)
			throw std::runtime_error("Terrain copy source metadata is missing or exceeds its bounded size.");
		std::string bytes(static_cast<std::size_t>(length), '\0'); input.seekg(0);
		if ((!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) || input.peek() != std::char_traits<char>::eof())
			throw std::runtime_error("Terrain copy source metadata changed or could not be captured completely.");
		return bytes;
	}

	// CURRENT contains two complete lines: the current hash and either a prior
	// hash or '-'. Two CRLF hash lines occupy 68 bytes; never trim away separators
	// or accidentally interpret the predecessor as part of the current identity.
	inline TerraEngine::Terrain::ContentHash ReadTerrainCopyRootHash(const std::filesystem::path& descriptor) {
		const auto bytes = ReadTerrainCopyMetadata(descriptor.string() + ".data/CURRENT", 68U);
		std::size_t offset = 0U;
		const auto line = [&]() -> std::string_view {
			const auto end = bytes.find('\n', offset);
			if (end == std::string::npos) throw std::runtime_error("Terrain copy source CURRENT is malformed.");
			auto result = std::string_view(bytes).substr(offset, end - offset); offset = end + 1U;
			if (!result.empty() && result.back() == '\r') result.remove_suffix(1U);
			return result;
		};
		const auto primary = line(); const auto predecessor = line();
		if (offset != bytes.size()) throw std::runtime_error("Terrain copy source CURRENT has trailing data.");
		const auto parseHash = [](std::string_view text) {
			if (text.size() != 32U) throw std::runtime_error("Terrain copy source CURRENT has an invalid content identity.");
			for (const char digit : text) if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
				throw std::runtime_error("Terrain copy source CURRENT has an invalid content identity.");
			TerraEngine::Terrain::ContentHash hash{};
			const auto high = std::from_chars(text.data(), text.data() + 16U, hash.high64, 16);
			const auto low = std::from_chars(text.data() + 16U, text.data() + 32U, hash.low64, 16);
			if (high.ec != std::errc{} || low.ec != std::errc{} || !hash.IsValid())
				throw std::runtime_error("Terrain copy source CURRENT has an invalid content identity.");
			return hash;
		};
		const auto hash = parseHash(primary);
		if (predecessor != "-") (void)parseHash(predecessor);
		return hash;
	}
}

