// TerrainLargeWorldFixture.cpp -- Deterministic sparse 256 km terrain fixture.
//
// The fixture describes a 256 x 256 km authored extent without materializing
// every possible sample area. A stable 16 x 16 coarse selection provides
// repeatable demand and fallback coverage while the manifest retains exact
// signed lattice coordinates for route and seam validation.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
	std::uint64_t Mix(std::uint64_t value) noexcept
	{
		value ^= value >> 30u;
		value *= 0xbf58476d1ce4e5b9ull;
		value ^= value >> 27u;
		value *= 0x94d049bb133111ebull;
		return value ^ (value >> 31u);
	}
}

int main(int argc, char** argv)
{
	std::filesystem::path output = "terrain-256km.json";
	for (int index = 1; index + 1 < argc; index += 2)
		if (std::string(argv[index]) == "--output") output = argv[index + 1];
	if (output.empty()) return 2;
	std::error_code error;
	std::filesystem::create_directories(output.parent_path(), error);
	std::ofstream file(output, std::ios::binary | std::ios::trunc);
	if (!file) return 1;

	file << "{\n  \"schemaVersion\": {\"major\": 1, \"minor\": 0},\n"
		<< "  \"fixtureId\": \"terrain-256km\",\n"
		<< "  \"extentMetres\": [256000, 256000],\n"
		<< "  \"sampleSpacingMetres\": 1,\n"
		<< "  \"areaSampleExtent\": 256,\n"
		<< "  \"coveragePolicy\": \"sparse-materialized-areas-with-implicit-defaults\",\n"
		<< "  \"materializedAreas\": [\n";
	bool first = true;
	for (std::int64_t z = -8; z < 8; ++z)
		for (std::int64_t x = -8; x < 8; ++x)
		{
			if (!first) file << ",\n";
			first = false;
			const auto identity = Mix(static_cast<std::uint64_t>((x + 8) * 31 + (z + 8)));
			file << "    {\"areaX\": " << x * 64 << ", \"areaZ\": " << z * 64
				<< ", \"contentHash\": \"" << std::hex << identity << std::dec << "\"}";
		}
	file << "\n  ],\n  \"expected\": {\n"
		<< "    \"fullExtentWithoutFullResidency\": true,\n"
		<< "    \"materializedAreaCount\": 256,\n"
		<< "    \"routeDistanceKilometres\": 1000,\n"
		<< "    \"coordinatePrecisionCentimetres\": 2\n"
		<< "  }\n}\n";
	return file.good() ? 0 : 1;
}
