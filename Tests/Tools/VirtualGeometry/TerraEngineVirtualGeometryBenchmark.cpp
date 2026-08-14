// TerraEngineVirtualGeometryBenchmark.cpp - Vulkan runtime-soak evidence writer.
//
// This executable deliberately does not fabricate a GPU measurement.  Instead,
// an installed Vulkan runtime adapter is launched for every sample and must
// publish a measured benchmark-capture document.  The adapter boundary keeps
// this portable tool independent from renderer process ownership while making
// missing, stale, or non-Vulkan evidence fail closed.

#include "VirtualGeometrySchemas.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
	using Json = nlohmann::ordered_json;
	using SteadyClock = std::chrono::steady_clock;

	struct BenchmarkOptions
	{
		std::filesystem::path output;
		std::filesystem::path workloadsPath;
		std::filesystem::path matrixPath;
		std::filesystem::path qualityPath;
		std::filesystem::path aggregateDirectory;
		std::filesystem::path runtimeAdapter;
		std::string workload = "baseline-processor-expanded";
		std::uint64_t durationSeconds = 0u;
		std::uint64_t sampleIntervalMilliseconds = 1000u;
	};

	struct RuntimeEvidence
	{
		Json environment;
		Json capabilities;
		Json identity;
		Json workload;
		Json visibilityAndShading;
		std::vector<Json> samples;
		std::string captureId;
		std::string executionId;
		std::string artifactSha256;
		std::string integritySha256;
		bool visualGatePassed = false;
	};

	std::string CurrentUtcCaptureTime()
	{
		const std::time_t now = std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now());
		std::tm utc{};
		// Windows and POSIX expose the same thread-safe UTC conversion with
		// reversed parameter order and different names. Keep both paths explicit
		// so evidence timestamps never fall back to the unsafe static buffer.
#if defined(_WIN32)
		gmtime_s(&utc, &now);
#else
		gmtime_r(&now, &utc);
#endif
		std::ostringstream value;
		value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
		return value.str();
	}

	std::string MakeExecutionId()
	{
		const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		return "vulkan-runtime-soak-" + std::to_string(ticks);
	}

	void WriteJson(const std::filesystem::path& path, const Json& value)
	{
		if (!path.parent_path().empty())
		{
			std::filesystem::create_directories(path.parent_path());
		}
		std::ofstream output(path, std::ios::binary);
		if (!output)
		{
			throw std::runtime_error("Unable to write " + path.string());
		}
		output << value.dump(2) << '\n';
	}

	Json ReadJson(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			throw std::runtime_error("Unable to read " + path.string());
		}
		Json value;
		input >> value;
		return value;
	}

	std::string ReadRuntimeAdapterRejectionReason(const std::filesystem::path& path)
	{
		// Adapters write their bounded rejection document before returning failure.
		// Preserve that concrete reason in the benchmark's unavailable result so a
		// missing crowd fixture is never collapsed into an ambiguous process error.
		try
		{
			const Json capture = ReadJson(path);
			const Json statistics = capture.value("statistics", Json::object());
			const Json reasons = statistics.value("rejectionReasons", Json::array());
			if(reasons.is_array() && !reasons.empty() && reasons.front().is_string())
			{
				return reasons.front().get<std::string>();
			}
			const Json runtimeSampling = capture.value("runtimeSampling", Json::object());
			const std::string reason = runtimeSampling.value("reason", std::string{});
			if(!reason.empty())
			{
				return reason;
			}
		}
		catch(const std::exception&)
		{
			// The caller will retain its process-level failure when the optional
			// adapter artifact is absent or malformed.
		}
		return {};
	}

	std::string LowerAscii(std::string value)
	{
		for (char& character : value)
		{
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}
		return value;
	}

	// Benchmark evidence is reviewed outside the engine, so use ordinary
	// SHA-256 digests that PowerShell, CI, and release tooling can reproduce.
	// This small local implementation avoids introducing a new runtime library
	// solely to hash the adapter artifacts already read by this executable.
	std::array<std::uint8_t, 32u> Sha256(std::span<const std::uint8_t> input)
	{
		constexpr std::array<std::uint32_t, 64u> roundConstants{
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
			0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
			0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
			0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
			0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
			0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
			0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
			0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
			0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
		std::array<std::uint32_t, 8u> state{
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

		if (input.size() > std::numeric_limits<std::uint64_t>::max() / 8u)
		{
			throw std::runtime_error("SHA-256 input is too large");
		}
		std::vector<std::uint8_t> padded(input.begin(), input.end());
		padded.push_back(0x80u);
		while ((padded.size() % 64u) != 56u)
		{
			padded.push_back(0u);
		}
		const std::uint64_t bitCount =
			static_cast<std::uint64_t>(input.size()) * 8u;
		for (int shift = 56; shift >= 0; shift -= 8)
		{
			padded.push_back(static_cast<std::uint8_t>(bitCount >> shift));
		}

		for (std::size_t blockOffset = 0u; blockOffset < padded.size();
			blockOffset += 64u)
		{
			std::array<std::uint32_t, 64u> words{};
			for (std::size_t index = 0u; index < 16u; ++index)
			{
				const std::size_t offset = blockOffset + index * 4u;
				words[index] =
					(static_cast<std::uint32_t>(padded[offset]) << 24u)
					| (static_cast<std::uint32_t>(padded[offset + 1u]) << 16u)
					| (static_cast<std::uint32_t>(padded[offset + 2u]) << 8u)
					| static_cast<std::uint32_t>(padded[offset + 3u]);
			}
			for (std::size_t index = 16u; index < words.size(); ++index)
			{
				const std::uint32_t first =
					std::rotr(words[index - 15u], 7)
					^ std::rotr(words[index - 15u], 18)
					^ (words[index - 15u] >> 3u);
				const std::uint32_t second =
					std::rotr(words[index - 2u], 17)
					^ std::rotr(words[index - 2u], 19)
					^ (words[index - 2u] >> 10u);
				words[index] = words[index - 16u] + first
					+ words[index - 7u] + second;
			}

			std::uint32_t a = state[0u];
			std::uint32_t b = state[1u];
			std::uint32_t c = state[2u];
			std::uint32_t d = state[3u];
			std::uint32_t e = state[4u];
			std::uint32_t f = state[5u];
			std::uint32_t g = state[6u];
			std::uint32_t h = state[7u];
			for (std::size_t index = 0u; index < words.size(); ++index)
			{
				const std::uint32_t choose = (e & f) ^ (~e & g);
				const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const std::uint32_t bigSigma0 =
					std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
				const std::uint32_t bigSigma1 =
					std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
				const std::uint32_t first = h + bigSigma1 + choose
					+ roundConstants[index] + words[index];
				const std::uint32_t second = bigSigma0 + majority;
				h = g;
				g = f;
				f = e;
				e = d + first;
				d = c;
				c = b;
				b = a;
				a = first + second;
			}
			state[0u] += a;
			state[1u] += b;
			state[2u] += c;
			state[3u] += d;
			state[4u] += e;
			state[5u] += f;
			state[6u] += g;
			state[7u] += h;
		}

		std::array<std::uint8_t, 32u> digest{};
		for (std::size_t index = 0u; index < state.size(); ++index)
		{
			digest[index * 4u] =
				static_cast<std::uint8_t>(state[index] >> 24u);
			digest[index * 4u + 1u] =
				static_cast<std::uint8_t>(state[index] >> 16u);
			digest[index * 4u + 2u] =
				static_cast<std::uint8_t>(state[index] >> 8u);
			digest[index * 4u + 3u] =
				static_cast<std::uint8_t>(state[index]);
		}
		return digest;
	}

	std::string Sha256Hex(std::span<const std::uint8_t> bytes)
	{
		const std::array<std::uint8_t, 32u> digest = Sha256(bytes);
		std::ostringstream value;
		value << std::hex << std::setfill('0');
		for (const std::uint8_t byte : digest)
		{
			value << std::setw(2) << static_cast<unsigned int>(byte);
		}
		return value.str();
	}

	std::string Sha256Hex(std::string_view text)
	{
		return Sha256Hex(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
	}

	std::string FileSha256Hex(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			throw std::runtime_error("Unable to hash " + path.string());
		}
		const std::vector<std::uint8_t> bytes{
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>()};
		return Sha256Hex(bytes);
	}

	bool IsSha256Hex(const std::string& value)
	{
		return value.size() == 64u
			&& std::all_of(value.begin(), value.end(), [](unsigned char character)
			{
				return std::isxdigit(character) != 0;
			});
	}

	void ValidateStableHashFields(
		const Json& expected,
		const Json& observed,
		std::span<const char* const> fields,
		std::string_view section)
	{
		for (const char* field : fields)
		{
			const std::string expectedHash =
				LowerAscii(expected.value(field, std::string{}));
			const std::string observedHash =
				LowerAscii(observed.value(field, std::string{}));
			if (!IsSha256Hex(observedHash))
			{
				throw std::runtime_error(
					"Runtime adapter " + std::string(section) + "." + field
					+ " is not a SHA-256 digest");
			}
			if (!expected.empty() && expectedHash != observedHash)
			{
				throw std::runtime_error(
					"Runtime adapter changed " + std::string(section) + "."
					+ field + " during one soak; mixed identities are rejected");
			}
		}
	}

	std::uint64_t ParsePositiveUnsigned(const std::string& text, const char* option)
	{
		std::size_t consumed = 0u;
		const unsigned long long parsed = std::stoull(text, &consumed);
		if (consumed != text.size() || parsed == 0u)
		{
			throw std::runtime_error(std::string(option) + " must be a positive integer");
		}
		return static_cast<std::uint64_t>(parsed);
	}

	std::string QuoteCommandArgument(const std::filesystem::path& value)
	{
		std::string quoted = "\"";
		for (const char character : value.string())
		{
			if (character == '\"')
			{
				quoted += "\\\"";
			}
			else
			{
				quoted.push_back(character);
			}
		}
		quoted += "\"";
		return quoted;
	}

	void ValidateWorkloadIdentifier(const std::string& workload)
	{
		// The adapter is launched through Windows command processing.  Keep the
		// user-selected workload to the manifest identifier alphabet so it cannot
		// alter the command line before the adapter receives it.
		const bool isSafeIdentifier = !workload.empty()
			&& std::all_of(workload.begin(), workload.end(), [](const unsigned char character)
			{
				return std::isalnum(character) || character == '-' || character == '_';
			});
		if (!isSafeIdentifier)
		{
			throw std::runtime_error("--workload must be a non-empty manifest identifier using letters, digits, '-' or '_'");
		}
	}

	Json NewUnavailableCapture(const std::string& workload, const std::string& reason)
	{
		Json capture = Json::object();
		capture["identity"] = {
			{"schemaVersion", "1"}, {"workloadVersion", "1"},
			{"captureId", "runtime-unavailable"}, {"runId", "runtime-unavailable"},
			{"executionId", "runtime-unavailable"}, {"captureTime", CurrentUtcCaptureTime()},
			{"engineHash", "not-captured"}, {"sourceHash", "not-captured"},
			{"diffHash", "not-captured"}, {"executableHash", "not-captured"},
			{"shaderHash", "not-captured"}, {"assetHash", "not-captured"},
			{"buildRecipeHash", "not-captured"}
		};
		capture["environment"] = {
			{"adapter", "unavailable"}, {"driver", "unavailable"},
			{"backend", "unavailable"}, {"operatingSystem", "not-probed"},
			{"route", "unavailable"}, {"resolution", "not-captured"},
			{"quality", "not-captured"}, {"viewCount", 0}
		};
		capture["workload"] = {{"name", workload}, {"sceneHash", "not-captured"}, {"workloadHash", "not-captured"}};
		capture["warmupAndSampling"] = {{"warmupSamples", 0}, {"requestedSamples", 0}, {"randomizedOrder", false}};
		capture["capabilities"] = Json::array();
		capture["rawSamples"] = Json::array();
		capture["statistics"] = {
			{"medianNanoseconds", nullptr}, {"p95Nanoseconds", nullptr},
			{"p99Nanoseconds", nullptr}, {"varianceNanosecondsSquared", nullptr},
			{"worstNanoseconds", nullptr}, {"sampleCount", 0}, {"rejectedCount", 0},
			{"boundedCounters", false}, {"rejectionReasons", Json::array({reason})}
		};
		for (const char* section : {
			"geometry", "residencyAndStreaming", "visibilityAndShading", "deformation",
			"auxiliaryViews", "fallbacksAndFailures", "baselineAndComparability", "artifacts"})
		{
			capture[section] = Json::object();
		}
		capture["baselineAndComparability"]["status"] = "unavailable";
		capture["integrity"] = {{"hash", "not-captured"}, {"diagnostics", {{"runtimeAdapter", reason}}}};
		capture["runtimeSampling"] = {{"status", "unavailable"}, {"reason", reason}};
		return capture;
	}

	std::vector<double> ReadMeasured(const std::filesystem::path& directory)
	{
		std::vector<double> values;
		for (const auto& entry : std::filesystem::directory_iterator(directory))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".json")
			{
				continue;
			}
			Json capture;
			try
			{
				capture = ReadJson(entry.path());
			}
			catch (const std::exception&)
			{
				continue;
			}
			if (!capture.contains("rawSamples") || !capture["rawSamples"].is_array())
			{
				continue;
			}
			const std::string executionId = capture.value("identity", Json::object()).value("executionId", std::string{});
			const std::string status = capture.value("baselineAndComparability", Json::object()).value("status", std::string{});
			if ((status != "measured" && status != "comparable") || executionId.empty() || executionId == "runtime-unavailable")
			{
				continue;
			}
			for (const Json& sample : capture["rawSamples"])
			{
				if (sample.value("availability", std::string{}) == "measured"
					&& sample.value("executionId", std::string{}) == executionId
					&& sample.contains("durationNanoseconds") && sample["durationNanoseconds"].is_number())
				{
					values.push_back(sample["durationNanoseconds"].get<double>());
				}
			}
		}
		return values;
	}

	double Percentile(std::vector<double> values, double fraction)
	{
		std::sort(values.begin(), values.end());
		const std::size_t index = static_cast<std::size_t>(std::ceil(fraction * values.size()) - 1.0);
		return values[std::min(index, values.size() - 1u)];
	}

	RuntimeEvidence ReadMeasuredVulkanRuntimeCapture(
		const std::filesystem::path& path,
		const std::string& benchmarkExecutionId,
		std::uint64_t& nextSampleIndex)
	{
		const Json capture = ReadJson(path);
		const Json identity = capture.value("identity", Json::object());
		const Json environment = capture.value("environment", Json::object());
		const Json statistics = capture.value("statistics", Json::object());
		const Json baseline = capture.value("baselineAndComparability", Json::object());
		const Json visibilityAndShading = capture.value(
			"visibilityAndShading", Json::object());
		const std::string captureId = identity.value("captureId", std::string{});
		const std::string captureExecutionId = identity.value("executionId", std::string{});
		const std::string backend = LowerAscii(environment.value("backend", std::string{}));
		const std::string adapter = environment.value("adapter", std::string{});
		const std::string driver = environment.value("driver", std::string{});
		const std::string status = baseline.value("status", std::string{});
		if (captureId.empty() || captureExecutionId.empty() || captureExecutionId == "runtime-unavailable")
		{
			throw std::runtime_error("Runtime adapter did not publish a concrete capture and execution identity: " + path.string());
		}
		if (backend != "vulkan" || adapter.empty() || adapter == "unavailable" || driver.empty() || driver == "unavailable")
		{
			throw std::runtime_error("Runtime adapter did not publish a measured Vulkan adapter and driver: " + path.string());
		}
		if ((status != "measured" && status != "comparable") || !statistics.value("boundedCounters", false))
		{
			throw std::runtime_error("Runtime adapter measurement is not bounded and accepted: " + path.string());
		}
		if (!visibilityAndShading.value("visualGatePassed", false))
		{
			throw std::runtime_error(
				"Runtime adapter visual validation did not pass: "
				+ path.string());
		}
		if (!capture.contains("rawSamples") || !capture["rawSamples"].is_array())
		{
			throw std::runtime_error("Runtime adapter did not publish raw samples: " + path.string());
		}

		RuntimeEvidence result;
		result.environment = environment;
		result.capabilities = capture.value("capabilities", Json::array());
		result.identity = identity;
		result.workload = capture.value("workload", Json::object());
		result.visibilityAndShading = visibilityAndShading;
		result.visualGatePassed = true;
		result.captureId = captureId;
		result.executionId = captureExecutionId;
		result.artifactSha256 = FileSha256Hex(path);
		result.integritySha256 = LowerAscii(capture.value(
			"integrity", Json::object()).value("hash", std::string{}));
		constexpr std::array identityHashFields{
			"engineHash", "sourceHash", "diffHash", "executableHash",
			"shaderHash", "assetHash", "buildRecipeHash"};
		constexpr std::array workloadHashFields{"sceneHash", "workloadHash"};
		ValidateStableHashFields(
			Json::object(), result.identity, identityHashFields, "identity");
		ValidateStableHashFields(
			Json::object(), result.workload, workloadHashFields, "workload");
		if (!IsSha256Hex(result.integritySha256))
		{
			throw std::runtime_error(
				"Runtime adapter integrity.hash is not a SHA-256 digest: "
				+ path.string());
		}
		for (const Json& sourceSample : capture["rawSamples"])
		{
			const std::string sourceExecutionId = sourceSample.value(
				"executionEvidenceId", sourceSample.value("executionId", std::string{}));
			if (sourceSample.value("availability", std::string{}) != "measured"
				|| sourceExecutionId.empty()
				|| !sourceSample.contains("durationNanoseconds")
				|| !sourceSample["durationNanoseconds"].is_number()
				|| sourceSample["durationNanoseconds"].get<double>() <= 0.0)
			{
				continue;
			}
			Json sample = sourceSample;
			sample["executionId"] = benchmarkExecutionId;
			sample["runtimeExecutionEvidenceId"] = sourceExecutionId;
			sample["runtimeCaptureId"] = captureId;
			sample["runtimeCapturePath"] = path.generic_string();
			sample["sampleIndex"] = ++nextSampleIndex;
			result.samples.push_back(std::move(sample));
		}
		if (result.samples.empty())
		{
			throw std::runtime_error("Runtime adapter capture contains no measured duration samples: " + path.string());
		}
		return result;
	}

	void ValidateMatchingRuntimeEnvironment(const Json& expected, const Json& observed)
	{
		for (const char* field : {"backend", "adapter", "driver", "route", "resolution", "quality"})
		{
			if (expected.value(field, std::string{}) != observed.value(field, std::string{}))
			{
				throw std::runtime_error(std::string("Runtime adapter changed ") + field + " during one soak; mixed environments are rejected.");
			}
		}
	}

	Json BuildMeasuredRuntimeSoak(const BenchmarkOptions& options)
	{
		if (options.durationSeconds == 0u)
		{
			throw std::runtime_error("--duration-seconds is required for runtime-backed sampling");
		}
		if (options.runtimeAdapter.empty() || !std::filesystem::exists(options.runtimeAdapter))
		{
			throw std::runtime_error("A runnable --runtime-adapter is required for Vulkan soak measurement");
		}
		ValidateWorkloadIdentifier(options.workload);

		const std::string benchmarkExecutionId = MakeExecutionId();
		std::filesystem::path runtimeCaptureDirectory = options.output;
		runtimeCaptureDirectory += ".runtime-captures";
		std::filesystem::create_directories(runtimeCaptureDirectory);
		const SteadyClock::time_point started = SteadyClock::now();
		const SteadyClock::time_point deadline = started + std::chrono::seconds(options.durationSeconds);
		std::uint64_t invocation = 0u;
		std::uint64_t sampleIndex = 0u;
		Json environment;
		Json capabilities = Json::array();
		Json runtimeIdentity;
		Json runtimeWorkload;
		Json rawSamples = Json::array();
		Json runtimeCaptureArtifacts = Json::array();
		Json runtimeVisualEvidence = Json::array();

		// Each invocation owns a distinct artifact.  Keeping these files lets a
		// reviewer trace every duration back to a real installed Vulkan capture.
		while (true)
		{
			++invocation;
			const std::filesystem::path adapterOutput = runtimeCaptureDirectory /
				("runtime-sample-" + std::to_string(invocation) + ".json");
			const std::string adapterCommand = QuoteCommandArgument(options.runtimeAdapter)
				+ " --output " + QuoteCommandArgument(adapterOutput)
				+ " --workload \"" + options.workload + "\" --backend \"vulkan\""
				+ " --sample-index " + std::to_string(invocation);
			// std::system delegates to cmd.exe on Windows.  The extra cmd envelope
			// keeps a quoted .cmd adapter path executable while preserving spaces in
			// installed-tree paths and in the per-sample output path.
			const std::string command = "cmd.exe /d /s /c \"" + adapterCommand + "\"";
			if (std::system(command.c_str()) != 0)
			{
				const std::string adapterReason =
					std::filesystem::exists(adapterOutput)
					? ReadRuntimeAdapterRejectionReason(adapterOutput)
					: std::string{};
				throw std::runtime_error(
					"Runtime Vulkan adapter rejected sample " + std::to_string(invocation)
					+ (adapterReason.empty() ? std::string{} : ": " + adapterReason));
			}
			if (!std::filesystem::exists(adapterOutput))
			{
				throw std::runtime_error("Runtime Vulkan adapter did not publish sample " + std::to_string(invocation));
			}

			RuntimeEvidence evidence = ReadMeasuredVulkanRuntimeCapture(adapterOutput, benchmarkExecutionId, sampleIndex);
			if (environment.empty())
			{
				environment = std::move(evidence.environment);
				capabilities = std::move(evidence.capabilities);
				runtimeIdentity = evidence.identity;
				runtimeWorkload = evidence.workload;
			}
			else
			{
				ValidateMatchingRuntimeEnvironment(environment, evidence.environment);
				constexpr std::array identityHashFields{
					"engineHash", "sourceHash", "diffHash", "executableHash",
					"shaderHash", "assetHash", "buildRecipeHash"};
				constexpr std::array workloadHashFields{"sceneHash", "workloadHash"};
				ValidateStableHashFields(
					runtimeIdentity, evidence.identity,
					identityHashFields, "identity");
				ValidateStableHashFields(
					runtimeWorkload, evidence.workload,
					workloadHashFields, "workload");
			}
			runtimeCaptureArtifacts.push_back({
				{"path", adapterOutput.generic_string()},
				{"sha256", evidence.artifactSha256},
				{"adapterIntegritySha256", evidence.integritySha256},
				{"captureId", evidence.captureId},
				{"executionId", evidence.executionId}});
			// Visual proof remains bound to each installed-runtime capture.  The
			// aggregate flag below is only true because every capture was checked
			// above, while this array lets reviewers inspect the original fields.
			runtimeVisualEvidence.push_back({
				{"captureId", evidence.captureId},
				{"executionId", evidence.executionId},
				{"visualGatePassed", evidence.visualGatePassed},
				{"evidence", evidence.visibilityAndShading}});
			for (Json& sample : evidence.samples)
			{
				rawSamples.push_back(std::move(sample));
			}

			const SteadyClock::time_point now = SteadyClock::now();
			if (now >= deadline)
			{
				break;
			}
			const SteadyClock::time_point nextSample = now + std::chrono::milliseconds(options.sampleIntervalMilliseconds);
			std::this_thread::sleep_until(std::min(nextSample, deadline));
		}

		if (rawSamples.empty())
		{
			throw std::runtime_error("Runtime Vulkan adapter completed without a measured sample");
		}
		const double elapsedSeconds = std::chrono::duration<double>(SteadyClock::now() - started).count();
		if (elapsedSeconds + 0.001 < static_cast<double>(options.durationSeconds))
		{
			throw std::runtime_error("Runtime sampling stopped before the requested duration elapsed");
		}

		std::vector<double> durations;
		durations.reserve(rawSamples.size());
		for (const Json& sample : rawSamples)
		{
			durations.push_back(sample["durationNanoseconds"].get<double>());
		}
		const double mean = std::accumulate(durations.begin(), durations.end(), 0.0) / static_cast<double>(durations.size());
		double variance = 0.0;
		for (const double duration : durations)
		{
			variance += (duration - mean) * (duration - mean);
		}
		variance /= static_cast<double>(durations.size());

		Json capture = Json::object();
		capture["identity"] = {
			{"schemaVersion", "1"}, {"workloadVersion", "1"},
			{"captureId", benchmarkExecutionId}, {"runId", benchmarkExecutionId},
			{"executionId", benchmarkExecutionId}, {"captureTime", CurrentUtcCaptureTime()},
			{"engineHash", LowerAscii(runtimeIdentity["engineHash"].get<std::string>())},
			{"sourceHash", LowerAscii(runtimeIdentity["sourceHash"].get<std::string>())},
			{"diffHash", LowerAscii(runtimeIdentity["diffHash"].get<std::string>())},
			{"executableHash", LowerAscii(runtimeIdentity["executableHash"].get<std::string>())},
			{"shaderHash", LowerAscii(runtimeIdentity["shaderHash"].get<std::string>())},
			{"assetHash", LowerAscii(runtimeIdentity["assetHash"].get<std::string>())},
			{"buildRecipeHash", LowerAscii(runtimeIdentity["buildRecipeHash"].get<std::string>())}
		};
		capture["environment"] = environment;
		capture["workload"] = {
			{"name", options.workload},
			{"sceneHash", LowerAscii(runtimeWorkload["sceneHash"].get<std::string>())},
			{"workloadHash", LowerAscii(runtimeWorkload["workloadHash"].get<std::string>())}};
		capture["warmupAndSampling"] = {{"warmupSamples", 0}, {"requestedSamples", rawSamples.size()}, {"randomizedOrder", false}};
		capture["capabilities"] = capabilities;
		capture["rawSamples"] = rawSamples;
		capture["statistics"] = {
			{"medianNanoseconds", Percentile(durations, 0.50)}, {"p95Nanoseconds", Percentile(durations, 0.95)},
			{"p99Nanoseconds", Percentile(durations, 0.99)}, {"varianceNanosecondsSquared", variance},
			{"worstNanoseconds", *std::max_element(durations.begin(), durations.end())}, {"sampleCount", durations.size()},
			{"rejectedCount", 0}, {"boundedCounters", true}, {"rejectionReasons", Json::array()}
		};
		capture["geometry"] = Json::object();
		capture["residencyAndStreaming"] = {{"boundedCounters", true}, {"runtimeSampleCount", rawSamples.size()}};
		capture["visibilityAndShading"] = {
			{"visualGatePassed", true},
			{"validatedRuntimeCaptureCount", runtimeVisualEvidence.size()},
			{"runtimeCaptures", runtimeVisualEvidence}};
		capture["deformation"] = Json::object();
		capture["auxiliaryViews"] = Json::object();
		capture["fallbacksAndFailures"] = {{"runtimeAdapterFailures", 0}};
		capture["baselineAndComparability"] = {{"status", "measured"}, {"source", "repeated-installed-vulkan-runtime-adapter"}};
		capture["artifacts"] = {
			{"runtimeCaptureDirectory", runtimeCaptureDirectory.generic_string()},
			{"runtimeCaptures", runtimeCaptureArtifacts}};
		capture["integrity"] = {
			{"hash", ""},
			{"diagnostics", {
				{"identityPolicy", "stable-sha256-values-preserved-from-runtime-adapter"},
				{"artifactPolicy", "each-runtime-capture-is-sha256-bound"},
				{"hashPolicy", "sha256-of-canonical-json-with-integrity-hash-empty"}}}};
		capture["runtimeSampling"] = {
			{"status", "measured"}, {"backend", "vulkan"}, {"requestedDurationSeconds", options.durationSeconds},
			{"actualDurationSeconds", elapsedSeconds}, {"adapterInvocations", invocation},
			{"runtimeCaptureDirectory", runtimeCaptureDirectory.generic_string()}
		};
		// Hash the complete canonical JSON while the hash field is empty. A
		// verifier can reproduce this value by clearing only integrity.hash;
		// measurement values are preserved exactly as reported by the adapter.
		capture["integrity"]["hash"] = Sha256Hex(capture.dump());
		return capture;
	}
}

int main(int argc, char** argv)
{
	BenchmarkOptions options;
	try
	{
		for (int index = 1; index < argc; ++index)
		{
			const std::string argument = argv[index];
			auto requireValue = [&](const char* option) -> std::string {
				if (index + 1 >= argc)
				{
					throw std::runtime_error(std::string("Missing value after ") + option);
				}
				return argv[++index];
			};
			if (argument == "--output") options.output = requireValue("--output");
			else if (argument == "--workloads") options.workloadsPath = requireValue("--workloads");
			else if (argument == "--matrix") options.matrixPath = requireValue("--matrix");
			else if (argument == "--quality") options.qualityPath = requireValue("--quality");
			else if (argument == "--workload") options.workload = requireValue("--workload");
			else if (argument == "--aggregate") options.aggregateDirectory = requireValue("--aggregate");
			else if (argument == "--runtime-adapter") options.runtimeAdapter = requireValue("--runtime-adapter");
			else if (argument == "--duration-seconds") options.durationSeconds = ParsePositiveUnsigned(requireValue("--duration-seconds"), "--duration-seconds");
			else if (argument == "--sample-interval-milliseconds") options.sampleIntervalMilliseconds = ParsePositiveUnsigned(requireValue("--sample-interval-milliseconds"), "--sample-interval-milliseconds");
			else if (argument == "--help")
			{
				std::cout << "--workloads PATH --matrix PATH --quality PATH --output PATH --workload ID --runtime-adapter PATH --duration-seconds N [--sample-interval-milliseconds N] | --aggregate DIR --output PATH\n";
				return 0;
			}
			else
			{
				throw std::runtime_error("Unrecognized argument: " + argument);
			}
		}
		if (options.output.empty())
		{
			throw std::runtime_error("--output is required");
		}
		const auto validate = [](const std::filesystem::path& path, TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind kind) {
			if (path.empty())
			{
				return;
			}
			const auto result = TerraEngine::Tools::VirtualGeometry::LoadAndValidateSchemaDocument(path, kind);
			if (!result.valid)
			{
				throw std::runtime_error("manifest validation failed: " + path.string());
			}
		};
		validate(options.workloadsPath, TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind::Workloads);
		validate(options.matrixPath, TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind::HardwareMatrix);
		validate(options.qualityPath, TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind::QualityProfiles);
		if (!options.aggregateDirectory.empty())
		{
			const std::vector<double> values = ReadMeasured(options.aggregateDirectory);
			if (values.size() < 5u)
			{
				throw std::runtime_error("at least five measured samples are required; unavailable/pending samples are excluded");
			}
			const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
			double variance = 0.0;
			for (const double value : values)
			{
				variance += (value - mean) * (value - mean);
			}
			variance /= static_cast<double>(values.size());
			WriteJson(options.output, {{"schemaVersion", 1}, {"sampleCount", values.size()},
				{"medianNanoseconds", Percentile(values, 0.50)}, {"p95Nanoseconds", Percentile(values, 0.95)},
				{"p99Nanoseconds", Percentile(values, 0.99)}, {"varianceNanosecondsSquared", variance},
				{"worstNanoseconds", *std::max_element(values.begin(), values.end())}});
			return 0;
		}

		WriteJson(options.output, BuildMeasuredRuntimeSoak(options));
		std::cout << "Wrote repeated Vulkan runtime soak capture.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Virtual geometry benchmark: " << error.what() << '\n';
		if (!options.output.empty())
		{
			try
			{
				WriteJson(options.output, NewUnavailableCapture(options.workload, error.what()));
			}
			catch (const std::exception& writeError)
			{
				std::cerr << "Virtual geometry benchmark could not write failure evidence: " << writeError.what() << '\n';
			}
		}
		return 2;
	}
}
