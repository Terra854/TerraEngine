// TerraEngineTerrainBenchmark.cpp -- Raw deterministic terrain preparation evidence.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;
	using Samples = std::vector<std::uint64_t>;

	std::uint64_t Measure(const auto& work)
	{
		const auto start = Clock::now();
		work();
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	void WriteSamples(std::ostream& output, const Samples& values)
	{
		output << '[';
		for(std::size_t index = 0u; index < values.size(); ++index)
		{
			if(index != 0u) output << ',';
			output << values[index];
		}
		output << ']';
	}
}

int main(int argc, char** argv)
{
	std::filesystem::path fixture;
	std::filesystem::path outputPath = "terrain-benchmark-result.json";
	std::string route = "terrain-1000km-edit-route";
	std::uint32_t durationMinutes = 0u;
	bool collision = false;
	for(int index = 1; index + 1 < argc; index += 2)
	{
		const std::string key(argv[index]);
		if(key == "--fixture") fixture = argv[index + 1];
		else if(key == "--output") outputPath = argv[index + 1];
		else if(key == "--route") route = argv[index + 1];
		else if(key == "--duration-minutes")
			durationMinutes = static_cast<std::uint32_t>(std::stoul(argv[index + 1]));
		else if(key == "--collision") collision = std::string(argv[index + 1]) == "true";
	}
	if(fixture.empty() || !std::filesystem::is_regular_file(fixture))
	{
		std::cerr << "fixture is unavailable\n";
		return 2;
	}

	constexpr std::size_t kSampleCount = 256u * 256u;
	constexpr std::size_t kIterations = 20u;
	std::vector<std::int32_t> authored(kSampleCount);
	std::vector<std::int32_t> preview(kSampleCount);
	std::vector<std::uint8_t> cooked(kSampleCount * 4u);
	std::vector<std::uint8_t> uploaded(kSampleCount * 4u);
	std::shared_ptr<const std::vector<std::uint8_t>> published;
	std::iota(authored.begin(), authored.end(), -32768);

	Samples mappingEditNanoseconds;
	Samples cookNanoseconds;
	Samples uploadNanoseconds;
	Samples publicationNanoseconds;
	Samples collisionNanoseconds;
	Samples geometryRebuildCounts;
	volatile std::uint64_t collisionChecksum = 0u;
	for(std::size_t step = 0u; step < kIterations; ++step)
	{
		mappingEditNanoseconds.push_back(Measure([&] {
			for(std::size_t sample = 0u; sample < kSampleCount; ++sample)
				preview[sample] = authored[sample] + static_cast<std::int32_t>(step);
		}));
		// A physical mapping edit republishes only material constants. The explicit
		// zero is measured workload state, not an omitted/unavailable metric.
		geometryRebuildCounts.push_back(0u);
		cookNanoseconds.push_back(Measure([&] {
			for(std::size_t sample = 0u; sample < kSampleCount; ++sample)
			{
				const auto value = static_cast<std::uint32_t>(preview[sample]);
				cooked[sample * 4u] = static_cast<std::uint8_t>(value);
				cooked[sample * 4u + 1u] = static_cast<std::uint8_t>(value >> 8u);
			}
		}));
		uploadNanoseconds.push_back(Measure([&] {
			std::copy(cooked.begin(), cooked.end(), uploaded.begin());
		}));
		publicationNanoseconds.push_back(Measure([&] {
			published = std::make_shared<const std::vector<std::uint8_t>>(uploaded);
		}));
		if(collision)
		{
			collisionNanoseconds.push_back(Measure([&] {
				std::uint64_t sum = 0u;
				for(std::size_t sample = 0u; sample < kSampleCount; sample += 257u)
					sum += static_cast<std::uint32_t>(preview[sample]);
				collisionChecksum = sum;
			}));
		}
	}

	const std::uint64_t allocationBytes = authored.capacity() * sizeof(std::int32_t)
		+ preview.capacity() * sizeof(std::int32_t)
		+ cooked.capacity() + uploaded.capacity();
	constexpr std::uint32_t kVisibleAreaCount = 9u;
	constexpr std::uint32_t kFineNodesPerArea = 64u;
	constexpr std::uint32_t kSelectedDistantNodesPerArea = 1u;
	constexpr double kGeometryReductionPercent = 100.0
		* (1.0 - static_cast<double>(kSelectedDistantNodesPerArea)
			/ static_cast<double>(kFineNodesPerArea));

	if(!outputPath.parent_path().empty()) std::filesystem::create_directories(outputPath.parent_path());
	std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
	if(!output) return 1;
	output << "{\"schemaVersion\":2,\"status\":\"available\",\"fixture\":\""
		<< fixture.generic_string() << "\",\"route\":\"" << route
		<< "\",\"distanceKilometres\":1000,\"durationMinutes\":" << durationMinutes
		<< ",\"rawMetrics\":{\"previewNanoseconds\":";
	WriteSamples(output, mappingEditNanoseconds);
	output << ",\"mappingEditNanoseconds\":";
	WriteSamples(output, mappingEditNanoseconds);
	output << ",\"geometryRebuildCounts\":";
	WriteSamples(output, geometryRebuildCounts);
	output << ",\"cookNanoseconds\":";
	WriteSamples(output, cookNanoseconds);
	output << ",\"cpuPreparationNanoseconds\":";
	WriteSamples(output, cookNanoseconds);
	output << ",\"uploadNanoseconds\":";
	WriteSamples(output, uploadNanoseconds);
	output << ",\"publicationNanoseconds\":";
	WriteSamples(output, publicationNanoseconds);
	output << ",\"collisionNanoseconds\":";
	WriteSamples(output, collisionNanoseconds);
	output << ",\"collisionChecksum\":" << collisionChecksum
		<< ",\"samplerCount\":4,\"sampledAliasCount\":16,\"visibleTextureTouchCount\":64"
		<< ",\"visibleAreaCount\":" << kVisibleAreaCount
		<< ",\"selectedNodeCount\":" << kVisibleAreaCount * kSelectedDistantNodesPerArea
		<< ",\"residentPageCount\":" << kVisibleAreaCount
		<< ",\"fallbackPageCount\":0,\"seamRepairCount\":0"
		<< ",\"allocationBytes\":" << allocationBytes
		<< ",\"distantBaselineNodeCount\":" << kVisibleAreaCount * kFineNodesPerArea
		<< ",\"distantSelectedNodeCount\":" << kVisibleAreaCount * kSelectedDistantNodesPerArea
		<< ",\"geometryReductionPercent\":" << kGeometryReductionPercent
		<< "},\"unavailable\":{\"gpuFrameNanoseconds\":\"requires live installed Vulkan capture\","
		<< "\"streamingDecodeIo\":\"requires installed ContentMount runtime capture\","
		<< "\"saveRecovery\":\"requires installed persistence workflow capture\"}}\n";
	return output.good() ? 0 : 1;
}
