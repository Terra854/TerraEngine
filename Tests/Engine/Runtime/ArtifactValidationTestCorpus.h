// ArtifactValidationTestCorpus.h - byte-authored images, never executable test code.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests::ArtifactCorpus {
	using Buffer = std::vector<std::byte>;
	// Fixed-size corpus writers fail loudly for incorrect test offsets rather
	// than silently repairing production-invalid image ranges.
	inline void Put(Buffer& bytes, std::size_t offset, std::uint64_t value, unsigned size) {
		for(unsigned i = 0; i < size; ++i) bytes.at(offset + i) = std::byte((value >> (i * 8)) & 255);
	}
	inline void Text(Buffer& bytes, std::size_t offset, std::string_view value) {
		for(std::size_t i = 0; i < value.size(); ++i) bytes.at(offset + i) = std::byte(static_cast<unsigned char>(value[i]));
		bytes.at(offset + value.size()) = std::byte{0};
	}
	inline Buffer Pe(std::string_view bootstrap = "InitManagedRuntimeV4") {
		Buffer bytes(0x800);
		Put(bytes, 0, 0x5a4d, 2); Put(bytes, 60, 0x80, 4);
		Put(bytes, 0x80, 0x4550, 4); Put(bytes, 0x84, 0x8664, 2);
		Put(bytes, 0x86, 1, 2); Put(bytes, 0x94, 240, 2); Put(bytes, 0x96, 0x2002, 2);
		const std::size_t optional = 0x98;
		Put(bytes, optional, 0x20b, 2); Put(bytes, optional + 24, 0x180000000, 8);
		Put(bytes, optional + 32, 0x1000, 4); Put(bytes, optional + 36, 0x200, 4);
		Put(bytes, optional + 56, 0x2000, 4); Put(bytes, optional + 60, 0x200, 4);
		Put(bytes, optional + 108, 16, 4); Put(bytes, optional + 112, 0x1100, 4); Put(bytes, optional + 116, 0x100, 4);
		const std::size_t section = optional + 240;
		Text(bytes, section, ".text"); Put(bytes, section + 8, 0x600, 4); Put(bytes, section + 12, 0x1000, 4);
		Put(bytes, section + 16, 0x600, 4); Put(bytes, section + 20, 0x200, 4); Put(bytes, section + 36, 0x60000020, 4);
		Put(bytes, 0x300 + 20, 1, 4); Put(bytes, 0x300 + 24, 1, 4);
		Put(bytes, 0x300 + 28, 0x1130, 4); Put(bytes, 0x300 + 32, 0x1140, 4); Put(bytes, 0x300 + 36, 0x1150, 4);
		Put(bytes, 0x330, 0x1020, 4); Put(bytes, 0x340, 0x1160, 4); Put(bytes, 0x350, 0, 2);
		Text(bytes, 0x360, bootstrap); Put(bytes, 0x220, 0xc3, 1);
		Text(bytes, 0x650, "legitimate PlaceholderDoor / pLaCeHoLdEr runtime data");
		return bytes;
	}
	inline Buffer Elf(std::string_view bootstrap = "InitManagedRuntimeV4", bool gnuHash = false) {
		Buffer bytes(0x600);
		constexpr std::uint64_t base = 0x400000;
		Put(bytes, 0, 0x464c457f, 4); Put(bytes, 4, 2, 1); Put(bytes, 5, 1, 1); Put(bytes, 6, 1, 1);
		Put(bytes, 16, 3, 2); Put(bytes, 18, 62, 2); Put(bytes, 20, 1, 4);
		Put(bytes, 32, 64, 8); Put(bytes, 52, 64, 2); Put(bytes, 54, 56, 2); Put(bytes, 56, 2, 2);
		// One executable load segment and one mapped dynamic segment suffice;
		// no section headers are required by a real ELF runtime loader.
		Put(bytes, 64, 1, 4); Put(bytes, 68, 5, 4); Put(bytes, 80, base, 8);
		Put(bytes, 96, bytes.size(), 8); Put(bytes, 104, bytes.size(), 8); Put(bytes, 112, 0x1000, 8);
		Put(bytes, 120, 2, 4); Put(bytes, 124, 4, 4); Put(bytes, 128, 0x180, 8); Put(bytes, 136, base + 0x180, 8);
		Put(bytes, 152, 96, 8); Put(bytes, 160, 96, 8); Put(bytes, 168, 8, 8);
		const auto dynamic = [&](std::size_t index, std::uint64_t tag, std::uint64_t value) {
			Put(bytes, 0x180 + index * 16, tag, 8); Put(bytes, 0x188 + index * 16, value, 8);
		};
		dynamic(0, 5, base + 0x280); dynamic(1, 10, bootstrap.size() + 2);
		dynamic(2, 6, base + 0x300); dynamic(3, 11, 24); dynamic(4, gnuHash ? 0x6ffffef5 : 4, base + 0x340); dynamic(5, 0, 0);
		Text(bytes, 0x281, bootstrap);
		Put(bytes, 0x318, 1, 4); Put(bytes, 0x31c, 0x12, 1); Put(bytes, 0x31e, 1, 2);
		Put(bytes, 0x320, base + 0x100, 8); Put(bytes, 0x328, 1, 8); Put(bytes, 0x100, 0xc3, 1);
		if(gnuHash) {
			Put(bytes, 0x340, 1, 4); Put(bytes, 0x344, 1, 4); Put(bytes, 0x348, 1, 4); Put(bytes, 0x34c, 5, 4);
			Put(bytes, 0x350, ~std::uint64_t{0}, 8); Put(bytes, 0x358, 1, 4); Put(bytes, 0x35c, 1, 4);
		} else {
			Put(bytes, 0x340, 1, 4); Put(bytes, 0x344, 2, 4); Put(bytes, 0x348, 1, 4);
		}
		Text(bytes, 0x420, "legitimate PlaceholderDoor / pLaCeHoLdEr runtime data");
		return bytes;
	}
}
