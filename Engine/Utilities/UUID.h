// UUID.h — Universally Unique Identifier (UUID) implementation.
//
// This file defines the UUID type used throughout the engine for identifying
// assets, entities, and objects. It includes utilities for generating new
// random UUIDs, parsing from strings, and serializing to hex strings, as well
// as an integration with std::hash for use in containers.

#pragma once

#include <array>
#include <random>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <optional>

namespace TerraEngine {

	using UUID = std::array<std::uint8_t, 16>;

	[[nodiscard]] inline UUID GenerateUUID() {
		// UUID creation is used by worker-side transaction producers. Each thread
		// owns its generator so concurrent jobs never race inside std::mt19937_64.
		thread_local std::mt19937_64 gen([] {
			std::random_device entropy;
			std::seed_seq seed{
				entropy(), entropy(), entropy(), entropy(),
				entropy(), entropy(), entropy(), entropy()};
			return std::mt19937_64(seed);
		}());

		UUID uuid{};

		std::uint64_t high = gen();
		std::uint64_t low = gen();

		std::memcpy(uuid.data(), &high, 8);
		std::memcpy(uuid.data() + 8, &low, 8);

		return uuid;
	}

	[[nodiscard]] inline std::string ToHexString(const UUID& uuid) {
		static const char hexChars[] = "0123456789abcdef";
		std::string s(32, '0');

		for (int i = 0; i < 16; ++i) {
			s[2 * i] = hexChars[(uuid[i] >> 4) & 0xF];
			s[2 * i + 1] = hexChars[uuid[i] & 0xF];
		}
		return s;
	}

	[[nodiscard]] inline std::optional<UUID> ParseUUID(std::string_view hex) {

		if (hex.size() != 32) return std::nullopt;
		UUID uuid{};
		for (int i = 0; i < 16; ++i) {
			auto fromHex = [](char c) -> int {

				if (c >= '0' && c <= '9') return c - '0';

				if (c >= 'a' && c <= 'f') return c - 'a' + 10;

				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};
			int h = fromHex(hex[2 * i]);
			int l = fromHex(hex[2 * i + 1]);

			if (h < 0 || l < 0) return std::nullopt;
			uuid[i] = static_cast<std::uint8_t>((h << 4) | l);
		}
		return uuid;
	}

	inline constexpr UUID EMPTY_UUID{};

} // namespace TerraEngine

namespace std {
	template<>

	struct hash<TerraEngine::UUID> {
		std::size_t operator()(const TerraEngine::UUID& uuid) const noexcept {
			std::uint64_t high;
			std::uint64_t low;

			std::memcpy(&high, uuid.data(), 8);
			std::memcpy(&low, uuid.data() + 8, 8);

			std::size_t h1 = std::hash<std::uint64_t>{}(high);
			std::size_t h2 = std::hash<std::uint64_t>{}(low);

			// Simple combination; you can replace with a stronger mix if desired
			return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
		}
	};
}
