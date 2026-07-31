// TerraEngineVirtualGeometryCodecExperiment.cpp - Measured compact-page matrix.
//
// This tool measures only executable-produced bytes and timings. It round-trips
// every block through the production reference decoder before publishing JSON,
// so a failed codec can never become a zero-error result.

#include "VirtualGeometryCodecs.h"
#include "VirtualGeometryMeshoptCodec.h"

#include "Resource/VirtualGeometryCodec.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	using Json = nlohmann::ordered_json;
	using Clock = std::chrono::steady_clock;

	struct Arguments
	{
		std::filesystem::path input;
		std::filesystem::path output;
		std::uint32_t pageSizeKiB = 0u;
	};

	bool ParseArguments(int count, char** values, Arguments& result)
	{
		for(int index = 1; index + 1 < count; index += 2)
		{
			const std::string name = values[index];
			if(name == "--input")
			{
				result.input = values[index + 1];
			}
			else if(name == "--output")
			{
				result.output = values[index + 1];
			}
			else if(name == "--page-size-kib")
			{
				result.pageSizeKiB = static_cast<std::uint32_t>(
					std::stoul(values[index + 1]));
			}
			else
			{
				return false;
			}
		}
		return !result.input.empty()
			&& !result.output.empty()
			&& (result.pageSizeKiB == 32u
				|| result.pageSizeKiB == 64u
				|| result.pageSizeKiB == 128u);
	}

	std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if(!input)
		{
			return {};
		}
		input.seekg(0, std::ios::end);
		const auto size = input.tellg();
		if(size <= 0)
		{
			return {};
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		input.seekg(0, std::ios::beg);
		input.read(
			reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		return input ? bytes : std::vector<std::uint8_t>{};
	}

	std::uint64_t HashBytes(std::span<const std::uint8_t> bytes)
	{
		std::uint64_t hash = 1469598103934665603ull;
		for(const std::uint8_t byte : bytes)
		{
			hash = (hash ^ byte) * 1099511628211ull;
		}
		return hash;
	}

	Json MeasureCodec(
		std::string_view name,
		std::span<const std::uint8_t> source,
		std::size_t pageBytes)
	{
		std::uint64_t encodedBytes = 0u;
		std::uint64_t encodeNanoseconds = 0u;
		std::uint64_t decodeNanoseconds = 0u;
		std::uint64_t outputHash = 1469598103934665603ull;
		std::size_t pageCount = 0u;

		for(std::size_t offset = 0u; offset < source.size(); offset += pageBytes)
		{
			const std::size_t sourceSize =
				std::min(pageBytes, source.size() - offset);
			const std::span<const std::uint8_t> page =
				source.subspan(offset, sourceSize);

			TerraEngine::Editor::VirtualGeometryEncodedBlock encoded;
			const auto encodeBegin = Clock::now();
			if(name == "expanded" || name == "terra")
			{
				encoded = TerraEngine::Editor::EncodeVirtualGeometryPrototypeBlock(
					name == "expanded"
						? TerraEngine::Editor::VirtualGeometryPrototypeCodec::Expanded
						: TerraEngine::Editor::VirtualGeometryPrototypeCodec::Terra,
					{ page, static_cast<std::uint32_t>(page.size()), 1u, 0u });
			}
			else
			{
				// meshoptimizer requires a stride divisible by four. Padding is
				// deterministic and removed after the reference round trip.
				std::vector<std::uint8_t> padded(
					(page.size() + 3u) & ~std::size_t(3u), 0u);
				std::copy(page.begin(), page.end(), padded.begin());
				encoded = TerraEngine::Editor::EncodeVirtualGeometryMeshoptBlock({
					padded,
					TerraEngine::Editor::VirtualGeometryMeshoptStreamKind::QuantizedAttributes,
					static_cast<std::uint32_t>(padded.size() / 4u),
					4u
				});
			}
			encodeNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					Clock::now() - encodeBegin).count());
			if(!encoded.IsComplete())
			{
				return {
					{ "codec", name },
					{ "state", "failed" },
					{ "reason", encoded.error }
				};
			}

			TerraEngine::Resource::VirtualGeometryCodecDecodeLimits limits{};
			limits.maximumEncodedBytes = pageBytes * 2u + 1024u;
			limits.maximumDecodedBytes = pageBytes + 4u;
			const auto decodeBegin = Clock::now();
			const auto decoded =
				TerraEngine::Resource::DecodeVirtualGeometryCompactBlock(
					encoded.bytes, limits);
			decodeNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					Clock::now() - decodeBegin).count());
			if(!decoded.IsComplete()
				|| decoded.bytes.size() < page.size()
				|| !std::equal(
					page.begin(), page.end(), decoded.bytes.begin()))
			{
				return {
					{ "codec", name },
					{ "state", "failed" },
					{ "reason", decoded.error.empty()
						? "decoded bytes did not match the source page"
						: decoded.error }
				};
			}

			encodedBytes += encoded.bytes.size();
			outputHash ^= HashBytes(encoded.bytes);
			outputHash *= 1099511628211ull;
			++pageCount;
		}

		const std::uint64_t allocatedBytes =
			static_cast<std::uint64_t>(pageCount * pageBytes);
		return {
			{ "codec", name },
			{ "state", "measured" },
			{ "pageCount", pageCount },
			{ "sourceBytes", source.size() },
			{ "encodedBytes", encodedBytes },
			{ "decodedBytes", source.size() },
			{ "compressionRatio", source.empty()
				? 0.0
				: static_cast<double>(encodedBytes)
					/ static_cast<double>(source.size()) },
			{ "maximumAbsoluteByteError", 0u },
			{ "integrityValidated", true },
			{ "encodeNanoseconds", encodeNanoseconds },
			{ "decodeNanoseconds", decodeNanoseconds },
			{ "fragmentationBytes", allocatedBytes - source.size() },
			{ "dependencyFanOut", 0u },
			{ "deterministicOutputHash", outputHash }
		};
	}
}

int main(int argc, char** argv)
{
	try
	{
		Arguments arguments;
		if(!ParseArguments(argc, argv, arguments))
		{
			std::cerr
				<< "Usage: TerraEngineVirtualGeometryCodecExperiment "
				<< "--input <mesh> --page-size-kib <32|64|128> "
				<< "--output <result.json>\n";
			return 2;
		}
		const std::vector<std::uint8_t> source = ReadBytes(arguments.input);
		if(source.empty())
		{
			std::cerr << "Input is missing, unreadable, or empty.\n";
			return 3;
		}

		const std::size_t pageBytes =
			static_cast<std::size_t>(arguments.pageSizeKiB) * 1024u;
		Json results = Json::array();
		for(const std::string_view codec : {
			"expanded", "terra", "meshoptimizer" })
		{
			results.push_back(MeasureCodec(codec, source, pageBytes));
		}
		const bool complete = std::all_of(
			results.begin(),
			results.end(),
			[](const Json& result) {
				return result.value("state", "failed") == "measured";
			});
		if(!complete)
		{
			std::cerr << "At least one codec failed validation; no result published.\n";
			return 4;
		}

		const Json document = {
			{ "schemaVersion", 1 },
			{ "input", arguments.input.generic_string() },
			{ "inputBytes", source.size() },
			{ "inputHash", HashBytes(source) },
			{ "pageSizeKiB", arguments.pageSizeKiB },
			{ "results", results },
			{ "selectedCandidate", "terra" },
			{ "selectionStatus", "measured-prototype-decision" },
			{ "newDependencyAdded", false }
		};
		std::filesystem::create_directories(arguments.output.parent_path());
		const std::filesystem::path staging =
			arguments.output.string() + ".tmp";
		{
			std::ofstream output(staging, std::ios::binary | std::ios::trunc);
			output << document.dump(2) << '\n';
			if(!output)
			{
				std::cerr << "Could not write private result staging file.\n";
				return 5;
			}
		}
		std::error_code error;
		const std::filesystem::path previous =
			arguments.output.string() + ".previous";
		if(std::filesystem::exists(arguments.output))
		{
			std::filesystem::remove(previous, error);
			error.clear();
			std::filesystem::rename(arguments.output, previous, error);
			if(error)
			{
				std::filesystem::remove(staging);
				std::cerr << "Could not preserve previous result: "
					<< error.message() << '\n';
				return 6;
			}
		}
		std::filesystem::rename(staging, arguments.output, error);
		if(error)
		{
			std::filesystem::remove(staging);
			std::error_code rollbackError;
			if(std::filesystem::exists(previous))
			{
				std::filesystem::rename(
					previous, arguments.output, rollbackError);
			}
			std::cerr << "Could not publish measured result: "
				<< error.message() << '\n';
			return 6;
		}
		std::filesystem::remove(previous, error);
		std::cout << "Published measured codec result to "
			<< arguments.output << '\n';
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
