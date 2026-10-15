// Native renderer lifecycle qualification; the fixture main owns CLI parsing.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace RuntimeMemorySafetyDraft {
	// Execute only an exact frozen renderer lifecycle lane in a fresh owned
	// project root, writing one fresh receipt. Returns0Passed/1Failed/2Unverified.
	int RunRendererMode(const std::filesystem::path& install,
		const std::filesystem::path& outputPath,
		const std::filesystem::path& projectRoot,
		std::string_view scenario, std::uint32_t iterations);
}
