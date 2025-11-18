#pragma once

#include <array>
#include <random>
#include <cstdint>
#include <cstring>
#include <functional>

namespace TerraEngine {

    using UUID = std::array<std::uint8_t, 16>;

    [[nodiscard]] inline UUID GenerateUUID() {
        static std::random_device rd;
        static std::mt19937_64 gen(rd());

        UUID uuid{};

        std::uint64_t high = gen();
        std::uint64_t low = gen();

        std::memcpy(uuid.data(), &high, 8);
        std::memcpy(uuid.data() + 8, &low, 8);

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
