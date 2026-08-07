// RendererHeterogeneousStaticBenchmark.cpp - typed renderer topology benchmark.
//
// This installed tool measures the engine's own immutable topology and dynamic
// grouping builders.  It intentionally does not create a synthetic container
// that resembles renderer work: every timed sample calls the same public API
// used by RenderSystem worker preparation.

#include "Renderer/Graph/RenderFrameSnapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	using Json = nlohmann::ordered_json;
	using Clock = std::chrono::steady_clock;

	constexpr std::uint32_t kRenderableCount = 4000u;

	struct Options
	{
		std::filesystem::path output;
		std::uint64_t durationSeconds = 60u;
		std::uint64_t sampleIntervalMilliseconds = 1000u;
	};

	// Encode the role and ordinal directly into a UUID.  Benchmark identity must
	// be repeatable: random UUIDs would conceal accidental material coalescing.
	TerraEngine::UUID MakeDeterministicId(
		std::uint64_t role,
		std::uint32_t ordinal) noexcept
	{
		TerraEngine::UUID id{};
		for(std::uint32_t byte = 0u; byte < 8u; ++byte)
		{
			id[byte] = static_cast<std::uint8_t>(role >> (byte * 8u));
		}
		for(std::uint32_t byte = 0u; byte < 4u; ++byte)
		{
			id[8u + byte] = static_cast<std::uint8_t>(ordinal >> (byte * 8u));
		}
		// The final bytes keep every generated identity non-empty even for ordinal
		// zero, which is important because empty IDs are renderer fallback inputs.
		id[12u] = 0x54u;
		id[13u] = 0x45u;
		id[14u] = 0x52u;
		id[15u] = 0x41u;
		return id;
	}

	TerraEngine::Renderer::RenderFrameSnapshot BuildSnapshot()
	{
		using namespace TerraEngine::Renderer;
		RenderFrameSnapshot snapshot{};
		snapshot.frameId = 1u;
		snapshot.sceneRevision = 1u;
		snapshot.resourceRevision = 1u;
		snapshot.renderables.reserve(kRenderableCount);
		const TerraEngine::UUID mesh = MakeDeterministicId(0x4d455348u, 1u);
		for(std::uint32_t ordinal = 0u; ordinal < kRenderableCount; ++ordinal)
		{
			RenderFrameRenderableRecord record{};
			record.objectId = MakeDeterministicId(0x4f424a45u, ordinal + 1u);
			record.meshId = mesh;
			// Each static renderable has its own material identity on purpose. This
			// exercises the renderer's no-peer fallback path instead of grouping.
			record.materialId = MakeDeterministicId(0x4d41544cu, ordinal + 1u);
			record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
			snapshot.renderables.push_back(record);
		}
		return snapshot;
	}

	std::uint64_t ElapsedNanoseconds(const Clock::time_point begin) noexcept
	{
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				Clock::now() - begin).count());
	}

	std::uint64_t Median(std::vector<std::uint64_t> values)
	{
		if(values.empty())
		{
			return 0u;
		}
		std::sort(values.begin(), values.end());
		return values[(values.size() - 1u) / 2u];
	}

	Options ParseOptions(const int argumentCount, char** arguments)
	{
		Options options{};
		for(int index = 1; index < argumentCount; ++index)
		{
			const std::string_view argument(arguments[index]);
			if(index + 1 >= argumentCount)
			{
				throw std::runtime_error("Missing value for " + std::string(argument));
			}
			const std::string_view value(arguments[++index]);
			if(argument == "--output")
			{
				options.output = std::filesystem::path(value);
			}
			else if(argument == "--duration-seconds")
			{
				options.durationSeconds = std::stoull(std::string(value));
			}
			else if(argument == "--sample-interval-milliseconds")
			{
				options.sampleIntervalMilliseconds = std::stoull(std::string(value));
			}
			else
			{
				throw std::runtime_error("Unknown argument " + std::string(argument));
			}
		}
		if(options.output.empty() || options.durationSeconds == 0u
			|| options.sampleIntervalMilliseconds == 0u)
		{
			throw std::runtime_error(
				"Usage: TerraEngineRendererTopologyBenchmark --output <path> "
				"[--duration-seconds <positive>] "
				"[--sample-interval-milliseconds <positive>]");
		}
		return options;
	}

	void WriteCapture(const Options& options)
	{
		using namespace TerraEngine::Renderer;
		const RenderFrameSnapshot snapshot = BuildSnapshot();
		const RenderFrameViewRequest view{
			RenderOutputKind::GameView,
			RenderViewType::Game,
			glm::uvec2(1280u, 720u),
			1u,
			1u };
		const Clock::time_point deadline = Clock::now()
			+ std::chrono::seconds(options.durationSeconds);
		Json samples = Json::array();
		std::vector<std::uint64_t> durations;
		std::uint64_t sampleOrdinal = 0u;
		const std::string executionId = "engine-renderer-topology-"
			+ std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
				Clock::now().time_since_epoch()).count());

		do
		{
			const Clock::time_point sampleBegin = Clock::now();
			const Clock::time_point topologyBegin = Clock::now();
			const PersistentRenderTopology topology = BuildPersistentRenderTopology(
				snapshot, sampleOrdinal + 1u);
			const std::uint64_t topologyNanoseconds = ElapsedNanoseconds(topologyBegin);
			const Clock::time_point groupingBegin = Clock::now();
			const DynamicRenderFrameState dynamic = BuildDynamicRenderFrameState(
				snapshot, topology, view, sampleOrdinal + 1u);
			const std::uint64_t groupingNanoseconds = ElapsedNanoseconds(groupingBegin);

			// A malformed topology result would make a captured number misleading, so
			// fail before publishing the document instead of quietly recording it.
			if(!topology.valid || topology.records.size() != kRenderableCount
				|| topology.groupTemplates.size() != kRenderableCount
				|| !dynamic.valid || !dynamic.groups.empty()
				|| dynamic.fallbackCommands.size() != kRenderableCount
				|| !dynamic.instances.empty())
			{
				throw std::runtime_error(
					"Renderer topology did not preserve the 4000 unique-material fallback workload");
			}

			const std::uint64_t durationNanoseconds = ElapsedNanoseconds(sampleBegin);
			durations.push_back(durationNanoseconds);
			samples.push_back({
				{ "availability", "measured" },
				{ "stage", "persistent-topology-and-dynamic-grouping" },
				{ "executionEvidenceId", executionId + "-sample-" + std::to_string(sampleOrdinal) },
				{ "durationNanoseconds", durationNanoseconds },
				{ "topologyNanoseconds", topologyNanoseconds },
				{ "groupingNanoseconds", groupingNanoseconds },
				{ "topologyRecordCount", topology.records.size() },
				{ "distinctMaterialGroupCount", topology.groupTemplates.size() },
				{ "fallbackCommandCount", dynamic.fallbackCommands.size() }
			});
			++sampleOrdinal;

			const std::uint64_t elapsedMilliseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					Clock::now() - sampleBegin).count());
			if(elapsedMilliseconds < options.sampleIntervalMilliseconds)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(
					options.sampleIntervalMilliseconds - elapsedMilliseconds));
			}
		} while(Clock::now() < deadline);

		Json capture{
			{ "identity", {
				{ "schemaVersion", "1" },
				{ "captureId", executionId },
				{ "executionId", executionId }
			} },
			{ "environment", {
				{ "backend", "engine-headless" },
				{ "route", "renderer-topology-api" }
			} },
			{ "workload", {
				{ "name", "heterogeneous-static" },
				{ "renderables", kRenderableCount },
				{ "distinctMaterialIdentities", kRenderableCount }
			} },
			{ "rawSamples", std::move(samples) },
			{ "statistics", {
				{ "sampleCount", durations.size() },
				{ "medianNanoseconds", Median(durations) },
				{ "boundedCounters", true }
			} },
			// This process performs no live command submission. Reporting a number
			// here would be fabricated GPU evidence, so callers receive a precise
			// unavailable state and can pair it with an installed-editor capture.
			{ "gpuTiming", {
				{ "availability", "unavailable" },
				{ "reason", "The headless topology benchmark does not submit a live renderer frame." }
			} }
		};

		if(!options.output.parent_path().empty())
		{
			std::filesystem::create_directories(options.output.parent_path());
		}
		std::ofstream output(options.output, std::ios::binary);
		if(!output)
		{
			throw std::runtime_error("Unable to write " + options.output.string());
		}
		output << capture.dump(2) << '\n';
	}
}

int main(const int argumentCount, char** arguments)
{
	try
	{
		WriteCapture(ParseOptions(argumentCount, arguments));
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "Renderer topology benchmark: " << error.what() << '\n';
		return 1;
	}
}
