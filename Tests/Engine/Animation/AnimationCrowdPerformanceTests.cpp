// AnimationCrowdPerformanceTests.cpp - Frozen crowd and hero benchmark schemas.
//
// This suite verifies source-controlled identities, counters, memory evidence,
// and tail-result validation only. It never measures a frame or decides final
// performance acceptance before ImplementationStage task ImplementationCheck.

#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Profiling;

	namespace {
		inline constexpr std::string_view kFrozenStressManifestSha256 =
			"b5c78e9f01dc0036db4fb4674f632ebf06a736e4a7c010ec56e1f3804b92131d";
		inline constexpr std::string_view kFrozenRuntimeContractSha256 =
			"774c6c7a40da4ebb8fa67feb3649e956235669428d4c50ad43cdb2dceca1a801";
		inline constexpr std::string_view kFrozenPerformanceWorkloadsSha256 =
			"add34eb7debacbaddb03c07de115cf00f7935a3e62e849cf4b370f2cbf1bd707";

		std::filesystem::path SourcePath(std::string_view relativePath)
		{
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR) / std::string(relativePath);
		}

		std::string ReadText(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				throw TestFailure("Could not read frozen crowd input: " + path.string());
			}
			std::string text{
				std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
			// Frozen source identities use repository LF bytes. Normalize a Windows
			// checkout's CRLF conversion so the content contract is platform-neutral.
			for(std::size_t position = 0; (position = text.find("\r\n", position)) != std::string::npos;) {
				text.erase(position, 1U);
			}
			return text;
		}

		constexpr std::uint32_t RotateRight(std::uint32_t value, std::uint32_t bits) noexcept
		{
			return (value >> bits) | (value << (32U - bits));
		}

		// Hashing the actual source bytes prevents a stale nearby literal from
		// blessing an edited fixture. This test-only implementation is portable and
		// avoids relying on an operating-system crypto provider.
		std::string Sha256(std::string_view input)
		{
			constexpr std::array<std::uint32_t, 64U> roundConstants = {
				0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
				0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
				0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
				0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
				0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
				0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
				0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
				0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
				0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
				0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
				0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
				0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
				0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
				0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
				0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
				0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
			std::array<std::uint32_t, 8U> state = {
				0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
				0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
			std::vector<std::uint8_t> bytes(input.begin(), input.end());
			const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8ULL;
			bytes.push_back(0x80U);
			while ((bytes.size() % 64U) != 56U) { bytes.push_back(0U); }
			for (int shift = 56; shift >= 0; shift -= 8)
			{
				bytes.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xffULL));
			}
			for (std::size_t block = 0U; block < bytes.size(); block += 64U)
			{
				std::array<std::uint32_t, 64U> words{};
				for (std::size_t index = 0U; index < 16U; ++index)
				{
					const std::size_t offset = block + index * 4U;
					words[index] = (static_cast<std::uint32_t>(bytes[offset]) << 24U)
						| (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U)
						| (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U)
						| static_cast<std::uint32_t>(bytes[offset + 3U]);
				}
				for (std::size_t index = 16U; index < words.size(); ++index)
				{
					const std::uint32_t s0 = RotateRight(words[index - 15U], 7U)
						^ RotateRight(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
					const std::uint32_t s1 = RotateRight(words[index - 2U], 17U)
						^ RotateRight(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
					words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
				}
				std::uint32_t a = state[0U];
				std::uint32_t b = state[1U];
				std::uint32_t c = state[2U];
				std::uint32_t d = state[3U];
				std::uint32_t e = state[4U];
				std::uint32_t f = state[5U];
				std::uint32_t g = state[6U];
				std::uint32_t h = state[7U];
				for (std::size_t round = 0U; round < roundConstants.size(); ++round)
				{
					const std::uint32_t sigma1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
					const std::uint32_t choose = (e & f) ^ ((~e) & g);
					const std::uint32_t first = h + sigma1 + choose + roundConstants[round] + words[round];
					const std::uint32_t sigma0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
					const std::uint32_t second = sigma0 + ((a & b) ^ (a & c) ^ (b & c));
					h = g; g = f; f = e; e = d + first; d = c; c = b; b = a; a = first + second;
				}
				state[0U] += a; state[1U] += b; state[2U] += c; state[3U] += d;
				state[4U] += e; state[5U] += f; state[6U] += g; state[7U] += h;
			}
			std::ostringstream output;
			output << std::hex << std::setfill('0');
			for (const std::uint32_t word : state) { output << std::setw(8) << word; }
			return output.str();
		}

		const nlohmann::json& FindWorkload(const nlohmann::json& manifest, std::string_view id)
		{
			for (const nlohmann::json& workload : manifest.at("workloads"))
			{
				if (workload.at("id").get<std::string>() == id) { return workload; }
			}
			throw TestFailure("Frozen stress workload was not found: " + std::string(id));
		}

		struct FrameTailV1
		{
			std::uint64_t sampleCount = 0U;
			double medianMilliseconds = 0.0;
			double p95Milliseconds = 0.0;
			double p99Milliseconds = 0.0;
			double worstMilliseconds = 0.0;
			double worstAnimationAttributedMilliseconds = 0.0;
		};

		struct ScaleCountersV1
		{
			std::uint64_t activeAnimators = 0U;
			std::uint64_t jointsPerAnimator = 0U;
			std::uint64_t blendedClipsPerAnimator = 0U;
			std::uint64_t cpuAnimators = 0U;
			std::uint64_t gpuVisualAnimators = 0U;
			std::uint64_t hybridAnimators = 0U;
			std::uint64_t gameCaptureViews = 0U;
			std::uint64_t shadowViews = 0U;
			std::uint64_t completePoseReadbackBytes = 0U;
			std::uint64_t animationGpuPoseWaits = 0U;
			std::uint64_t currentMemoryBytes = 0U;
			std::uint64_t highWaterMemoryBytes = 0U;
		};

		struct ScaleBenchmarkResultV1
		{
			std::uint32_t schemaVersion = 1U;
			std::uint32_t phase = 18U;
			std::string profileId;
			std::string fixtureSha256 = std::string(kFrozenStressManifestSha256);
			bool timedAcceptanceAttempted = false;
			bool referenceIdentityMatched = false;
			bool memoryBoundedAfterWarmup = false;
			double updatesPerSecond = 0.0;
			ScaleCountersV1 counters;
			std::vector<FrameTailV1> pairedRuns;
		};

		enum class CrowdVariation : std::uint8_t { Shared, PhaseOffset, MixedClip, Heterogeneous };

		struct CrowdAnimatorScenario
		{
			std::uint32_t clip = 0U;
			std::uint32_t phaseTick = 0U;
			CrowdVariation variation = CrowdVariation::Shared;
		};

		std::vector<CrowdAnimatorScenario> BuildCrowdScenario(CrowdVariation variation)
		{
			std::vector<CrowdAnimatorScenario> animators;
			animators.reserve(4000U);
			for (std::uint32_t index = 0U; index < 4000U; ++index)
			{
				// The four profiles isolate sharing, phase, clip selection, and all
				// combined variation while remaining reproducible across worker counts.
				CrowdAnimatorScenario scenario;
				scenario.variation = variation;
				scenario.clip = variation == CrowdVariation::Shared || variation == CrowdVariation::PhaseOffset
					? 0U : (variation == CrowdVariation::MixedClip ? index % 4U : (index * 17U) % 29U);
				scenario.phaseTick = variation == CrowdVariation::Shared ? 0U
					: (variation == CrowdVariation::PhaseOffset ? index % 60U : (index * 37U) % 240U);
				animators.push_back(scenario);
			}
			return animators;
		}

		bool ValidateTail(const FrameTailV1& tail)
		{
			return tail.sampleCount > 0U && std::isfinite(tail.medianMilliseconds)
				&& std::isfinite(tail.p95Milliseconds) && std::isfinite(tail.p99Milliseconds)
				&& std::isfinite(tail.worstMilliseconds) && std::isfinite(tail.worstAnimationAttributedMilliseconds)
				&& tail.medianMilliseconds >= 0.0 && tail.p95Milliseconds >= tail.medianMilliseconds
				&& tail.p99Milliseconds >= tail.p95Milliseconds && tail.worstMilliseconds >= tail.p99Milliseconds
				&& tail.worstAnimationAttributedMilliseconds >= 0.0;
		}

		bool ValidateScaleBenchmark(const ScaleBenchmarkResultV1& result, bool finalPhaseAcceptanceAllowed)
		{
			if (result.schemaVersion != 1U || result.phase != 18U || !result.timedAcceptanceAttempted
				|| !finalPhaseAcceptanceAllowed || !result.referenceIdentityMatched
				|| result.fixtureSha256 != kFrozenStressManifestSha256
				|| !result.memoryBoundedAfterWarmup || !std::isfinite(result.updatesPerSecond)
				|| result.updatesPerSecond < 60.0 || result.pairedRuns.size() < 5U
				|| result.counters.activeAnimators == 0U
				|| result.counters.currentMemoryBytes > result.counters.highWaterMemoryBytes
				|| result.counters.completePoseReadbackBytes != 0U
				|| result.counters.animationGpuPoseWaits != 0U)
			{
				return false;
			}
			if (result.counters.cpuAnimators > result.counters.activeAnimators
				|| result.counters.gpuVisualAnimators > result.counters.activeAnimators - result.counters.cpuAnimators
				|| result.counters.hybridAnimators > result.counters.activeAnimators
					- result.counters.cpuAnimators - result.counters.gpuVisualAnimators)
			{
				return false;
			}
			for (const FrameTailV1& tail : result.pairedRuns)
			{
				if (!ValidateTail(tail)) { return false; }
				if ((result.profileId == "crowd-1000" || result.profileId == "crowd-4000")
					&& tail.worstAnimationAttributedMilliseconds > 50.0) { return false; }
			}
			if (result.profileId == "crowd-1000")
			{
				return result.counters.activeAnimators == 1000U
					&& result.counters.jointsPerAnimator == 100U
					&& result.counters.blendedClipsPerAnimator == 2U
					&& result.counters.gameCaptureViews == 1U
					&& result.counters.shadowViews == 1U;
			}
			if (result.profileId == "complex-hero")
			{
				return result.counters.activeAnimators == 1U
					&& result.counters.jointsPerAnimator == 256U
					&& result.counters.gameCaptureViews == 2U
					&& result.counters.shadowViews >= 1U;
			}
			if (result.profileId == "crowd-4000")
			{
				return result.counters.activeAnimators == 4000U
					&& result.counters.jointsPerAnimator == 100U
					&& result.counters.blendedClipsPerAnimator == 2U
					&& result.counters.gameCaptureViews == 1U
					&& result.counters.shadowViews == 1U;
			}
			return false;
		}
	}

	void RegisterAnimationCrowdPerformanceTests(TestRegistry& registry)
	{
		registry.Add("Animation.CrowdPerformance.FrozenManifestAndAuthoritiesMatchBytes", [](TestContext& context) {
			const std::string manifestText = ReadText(SourcePath("Tests/Fixtures/Animation/stress/workloads.json"));
			const nlohmann::json manifest = nlohmann::json::parse(manifestText);
			context.ExpectEqual(std::string(kFrozenStressManifestSha256), Sha256(manifestText),
				"The complete generated-workload manifest must retain its reviewed byte hash.");
			context.Expect(manifest.at("performanceAcceptanceMode") == "release"
				&& manifest.at("performanceExecutionDeferred").get<bool>(),
				"Scale performance execution must remain deferred to final ImplementationStage.");
			const nlohmann::json& runtime = manifest.at("authority").at("runtimeContract");
			const nlohmann::json& performance = manifest.at("authority").at("performanceWorkloads");
			context.Expect(runtime.at("sha256").get<std::string>() == kFrozenRuntimeContractSha256
				&& Sha256(ReadText(SourcePath(runtime.at("path").get<std::string>()))) == kFrozenRuntimeContractSha256,
				"The runtime profile path and bytes must match the frozen authority.");
			context.Expect(performance.at("sha256").get<std::string>() == kFrozenPerformanceWorkloadsSha256
				&& Sha256(ReadText(SourcePath(performance.at("path").get<std::string>()))) == kFrozenPerformanceWorkloadsSha256,
				"The immutable ImplementationCheck performance workload bytes must remain unchanged.");
		});

		registry.Add("Animation.CrowdPerformance.CrowdDefinitionIsExact", [](TestContext& context) {
			const nlohmann::json manifest = nlohmann::json::parse(ReadText(SourcePath("Tests/Fixtures/Animation/stress/workloads.json")));
			const nlohmann::json& workload = FindWorkload(manifest, "crowd-1000");
			const nlohmann::json& definition = workload.at("definition");
			context.Expect(workload.at("definitionSha256") == "11bfd18d158d96a9d48964c15f9db2771c2ccf3eeda90a039f9c48b9224779cb",
				"The canonical crowd definition hash must not drift.");
			context.Expect(definition.at("activeCharacters") == 1000
				&& definition.at("jointsPerCharacter") == 100
				&& definition.at("blendedClipsPerCharacter") == 2
				&& definition.at("morphTargetsPerCharacter") == 4
				&& definition.at("shadowCastingViews") == 1
				&& definition.at("measuredSecondsAfterWarmup") == 60
				&& definition.at("pairedMeasuredRuns") == 5,
				"The crowd content, view, duration, and pairing profile must remain exact.");
		});

		registry.Add("Animation.CrowdPerformance.HeroDefinitionCoversAllComplexFeatures", [](TestContext& context) {
			const nlohmann::json manifest = nlohmann::json::parse(ReadText(SourcePath("Tests/Fixtures/Animation/stress/workloads.json")));
			const nlohmann::json& workload = FindWorkload(manifest, "complex-hero");
			const nlohmann::json& definition = workload.at("definition");
			context.Expect(workload.at("definitionSha256") == "9f4f2a0b24b102c36ab651ca6a628496830e0c8a4c78644442aadf3f4506b150",
				"The canonical hero definition hash must not drift.");
			context.Expect(definition.at("sourceJoints") == 256
				&& definition.at("palettePartitionLimit") == 256
				&& definition.at("layeredController").get<bool>()
				&& definition.at("activeMorphTargets") == 2
				&& definition.at("ikChains") == 2
				&& definition.at("motionMatching").get<bool>()
				&& definition.at("deformableVirtualGeometry").get<bool>()
				&& definition.at("partialRagdoll").get<bool>()
				&& definition.at("gameCaptureViews") == 2
				&& definition.at("shadows").get<bool>(),
				"The complex hero must retain layered, morph, IK, motion, VG, ragdoll, multiview, and shadow work.");
		});

		registry.Add("Animation.CrowdPerformance.ResultSchemaIsFinalOnlyAndFailClosed", [](TestContext& context) {
			ScaleBenchmarkResultV1 result;
			result.profileId = "crowd-1000";
			result.timedAcceptanceAttempted = true;
			result.referenceIdentityMatched = true;
			result.memoryBoundedAfterWarmup = true;
			result.updatesPerSecond = 60.0;
			result.counters = ScaleCountersV1{1000U, 100U, 2U, 250U, 500U, 250U, 1U, 1U, 0U, 0U, 128U, 256U};
			result.pairedRuns.assign(5U, FrameTailV1{3600U, 10.0, 12.0, 14.0, 16.0, 50.0});
			context.Expect(!ValidateScaleBenchmark(result, false),
				"A complete synthetic result must not pass before final ImplementationStage.");
			context.Expect(ValidateScaleBenchmark(result, true),
				"Exact-boundary mixed-mode, bounded-memory, and tail evidence should satisfy the final schema.");
			result.pairedRuns[0U].p99Milliseconds = std::numeric_limits<double>::quiet_NaN();
			context.Expect(!ValidateScaleBenchmark(result, true),
				"A missing tail statistic must fail closed.");
			result.pairedRuns[0U] = FrameTailV1{3600U, 10.0, 12.0, 14.0, 51.0, 50.01};
			context.Expect(!ValidateScaleBenchmark(result, true),
				"A crowd animation frame above 50 milliseconds must fail.");
		});

		registry.Add("Animation.CrowdPerformance.FourThousandDeterministicVariations", [](TestContext& context) {
			const auto shared = BuildCrowdScenario(CrowdVariation::Shared);
			const auto phaseOffset = BuildCrowdScenario(CrowdVariation::PhaseOffset);
			const auto mixedClip = BuildCrowdScenario(CrowdVariation::MixedClip);
			const auto heterogeneous = BuildCrowdScenario(CrowdVariation::Heterogeneous);
			const auto repeatedHeterogeneous = BuildCrowdScenario(CrowdVariation::Heterogeneous);
			const bool heterogeneousRepeatable = std::equal(heterogeneous.begin(), heterogeneous.end(),
				repeatedHeterogeneous.begin(), [](const CrowdAnimatorScenario& left,
					const CrowdAnimatorScenario& right) {
					return left.clip == right.clip && left.phaseTick == right.phaseTick
						&& left.variation == right.variation;
				});
			context.Expect(shared.size() == 4000U && phaseOffset.size() == 4000U
				&& mixedClip.size() == 4000U && heterogeneous.size() == 4000U
				&& shared.front().clip == shared.back().clip && shared.front().phaseTick == 0U
				&& phaseOffset[1U].phaseTick != phaseOffset[0U].phaseTick
				&& mixedClip[0U].clip != mixedClip[1U].clip
				&& heterogeneous[17U].clip != heterogeneous[18U].clip
				&& heterogeneous[17U].phaseTick != heterogeneous[18U].phaseTick
				&& heterogeneousRepeatable,
				"All four 4,000-animator stress populations must preserve their intended sharing and variation identities.");

			ScaleBenchmarkResultV1 result;
			result.profileId = "crowd-4000";
			result.timedAcceptanceAttempted = true;
			result.referenceIdentityMatched = true;
			result.memoryBoundedAfterWarmup = true;
			result.updatesPerSecond = 60.0;
			result.counters = ScaleCountersV1{4000U, 100U, 2U, 1000U, 2000U, 1000U, 1U, 1U, 0U, 0U, 512U, 1024U};
			result.pairedRuns.assign(5U, FrameTailV1{3600U, 10.0, 12.0, 14.0, 16.0, 50.0});
			context.Expect(ValidateScaleBenchmark(result, true),
				"The 4,000-animator mixed-mode result must use the same fail-closed tail and bounded-memory schema.");
		});

		registry.Add("Animation.CrowdPerformance.ProfilerSummaryCarriesMixedModeMemoryAndViews", [](TestContext& context) {
			ProfilerAnimationFrameTelemetry telemetry;
			ProfilerAnimationFrameSummary& summary = telemetry.summary;
			summary.availableEvidenceMask = kProfilerAnimationAllEvidence;
			summary.schedulerMembershipChecks = 1U;
			summary.activeAnimators = 1000U;
			summary.evaluatedAnimators = 1000U;
			summary.cpuAnimators = 250U;
			summary.gpuVisualAnimators = 500U;
			summary.hybridAnimators = 250U;
			summary.workerCount = 31U;
			summary.workerJobs = 31U;
			summary.workerUsefulItems = 1000U;
			summary.workerCapacityNanoseconds = 100000U;
			summary.workerBusyNanoseconds = 75000U;
			summary.sampledJoints = 25000U;
			summary.blendedTracks = 2000U;
			summary.gpuEvaluatedJoints = 75000U;
			summary.gpuBatches = 16U;
			summary.frameArenaBytes = 1048576U;
			summary.frameArenaHighWaterBytes = 2097152U;
			summary.poseCacheBytes = 4194304U;
			summary.poseCacheHighWaterBytes = 8388608U;
			summary.gpuResidentBytes = 16777216U;
			summary.gpuResidentHighWaterBytes = 33554432U;
		context.Expect(ValidateProfilerAnimationFrameTelemetry(telemetry),
				"The profiler must represent mixed CPU/GPU/hybrid volume and bounded memory without per-instance detail.");
			summary.gpuResidentBytes = summary.gpuResidentHighWaterBytes + 1U;
			context.Expect(!ValidateProfilerAnimationFrameTelemetry(telemetry),
				"Current GPU residency above its high-water bound must be rejected.");
		});
	}
}
