// GpuAnimationPerformanceGateTests.cpp - Non-timed GPU gate contract tests.
//
// ImplementationStage freezes the workload, counters, and evidence shape but deliberately
// does not measure throughput. The real wall-clock comparisons run only in
// final ImplementationStage, when the complete animation feature and reference host are
// available. These tests therefore validate identities and zero-stall safety
// without turning a developer machine into performance evidence.

#include "TestHarness.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		inline constexpr std::string_view kFrozenWorkloadHash =
			"add34eb7debacbaddb03c07de115cf00f7935a3e62e849cf4b370f2cbf1bd707";
		inline constexpr std::string_view kFrozenRuntimeContractHash =
			"774c6c7a40da4ebb8fa67feb3649e956235669428d4c50ad43cdb2dceca1a801";

		std::filesystem::path SourcePath(std::string_view relativePath)
		{
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR) / std::string(relativePath);
		}

		std::string ReadText(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if(!stream)
			{
				throw TestFailure("Could not read frozen animation gate input: " + path.string());
			}
			std::string text{
				std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>()};
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

		// This small test-only SHA-256 implementation hashes source-controlled gate
		// inputs on every run. It avoids a platform crypto dependency and catches a
		// changed workload file even when someone forgets to update a nearby literal.
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
			while((bytes.size() % 64U) != 56U)
			{
				bytes.push_back(0U);
			}
			for(int shift = 56; shift >= 0; shift -= 8)
			{
				bytes.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xffULL));
			}

			for(std::size_t block = 0U; block < bytes.size(); block += 64U)
			{
				std::array<std::uint32_t, 64U> words{};
				for(std::size_t index = 0U; index < 16U; ++index)
				{
					const std::size_t offset = block + index * 4U;
					words[index] = (static_cast<std::uint32_t>(bytes[offset]) << 24U)
						| (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U)
						| (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U)
						| static_cast<std::uint32_t>(bytes[offset + 3U]);
				}
				for(std::size_t index = 16U; index < words.size(); ++index)
				{
					const std::uint32_t s0 = RotateRight(words[index - 15U], 7U)
						^ RotateRight(words[index - 15U], 18U)
						^ (words[index - 15U] >> 3U);
					const std::uint32_t s1 = RotateRight(words[index - 2U], 17U)
						^ RotateRight(words[index - 2U], 19U)
						^ (words[index - 2U] >> 10U);
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
				for(std::size_t round = 0U; round < roundConstants.size(); ++round)
				{
					const std::uint32_t upperSigma1 = RotateRight(e, 6U)
						^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
					const std::uint32_t choose = (e & f) ^ ((~e) & g);
					const std::uint32_t temporary1 = h + upperSigma1 + choose
						+ roundConstants[round] + words[round];
					const std::uint32_t upperSigma0 = RotateRight(a, 2U)
						^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
					const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
					const std::uint32_t temporary2 = upperSigma0 + majority;
					h = g;
					g = f;
					f = e;
					e = d + temporary1;
					d = c;
					c = b;
					b = a;
					a = temporary1 + temporary2;
				}
				state[0U] += a;
				state[1U] += b;
				state[2U] += c;
				state[3U] += d;
				state[4U] += e;
				state[5U] += f;
				state[6U] += g;
				state[7U] += h;
			}

			std::ostringstream output;
			output << std::hex << std::setfill('0');
			for(const std::uint32_t word : state)
			{
				output << std::setw(8) << word;
			}
			return output.str();
		}

		enum class GpuGateRunKind : std::uint8_t {
			ContractOnly = 0U,
			TimedPhase18 = 1U
		};

		// Every field is required in the final JSON result even when its value is
		// zero. Optional values are reserved for measurements that must remain absent
		// before ImplementationStage, preventing a throttled workstation from accepting a gate.
		struct GpuGateCountersV1 {
			std::uint64_t activeCharacters = 0U;
			std::uint64_t jointsPerCharacter = 0U;
			std::uint64_t blendedClipsPerCharacter = 0U;
			std::uint64_t cpuSampledJoints = 0U;
			std::uint64_t gpuDispatches = 0U;
			std::uint64_t completePoseReadbacks = 0U;
			std::uint64_t completePoseReadbackBytes = 0U;
			std::uint64_t animationGpuPoseWaits = 0U;
			std::uint64_t residencyMisses = 0U;
			std::uint64_t residentBytes = 0U;
		};

		struct GpuGateResultV1 {
			std::uint32_t schemaVersion = 1U;
			GpuGateRunKind runKind = GpuGateRunKind::ContractOnly;
			std::string_view feature = kAnimationFeatureIdentity;
			std::string_view profile = "crowd";
			std::string_view workloadsSha256 = kAnimationPerformanceWorkloadsSha256;
			bool timedAcceptanceAttempted = false;
			GpuGateCountersV1 counters{};
			std::optional<double> updatesPerSecond;
			std::optional<double> maximumAnimationFrameMilliseconds;
			std::optional<double> gpuVisualCpuReductionPercent;
		};

		bool IsFiniteMeasurement(const std::optional<double>& value) noexcept
		{
			return value.has_value() && std::isfinite(*value) && *value >= 0.0;
		}

		bool ValidateGpuGateResult(const GpuGateResultV1& result, bool finalPhase) noexcept
		{
			if(result.schemaVersion != 1U
				|| result.feature != kAnimationFeatureIdentity
				|| result.profile != "crowd"
				|| result.workloadsSha256 != kFrozenWorkloadHash
				|| result.counters.activeCharacters != 1000U
				|| result.counters.jointsPerCharacter != 100U
				|| result.counters.blendedClipsPerCharacter != 2U
				|| result.counters.completePoseReadbacks != 0U
				|| result.counters.completePoseReadbackBytes != 0U
				|| result.counters.animationGpuPoseWaits != 0U)
			{
				return false;
			}

			if(result.runKind == GpuGateRunKind::ContractOnly)
			{
				return !result.timedAcceptanceAttempted
					&& !result.updatesPerSecond.has_value()
					&& !result.maximumAnimationFrameMilliseconds.has_value()
					&& !result.gpuVisualCpuReductionPercent.has_value();
			}

			return finalPhase
				&& result.timedAcceptanceAttempted
				&& IsFiniteMeasurement(result.updatesPerSecond)
				&& IsFiniteMeasurement(result.maximumAnimationFrameMilliseconds)
				&& IsFiniteMeasurement(result.gpuVisualCpuReductionPercent)
				&& *result.updatesPerSecond >= 60.0
				&& *result.maximumAnimationFrameMilliseconds <= 50.0
				&& *result.gpuVisualCpuReductionPercent >= 75.0;
		}

		const nlohmann::json& FindProfile(const nlohmann::json& document, std::string_view id)
		{
			for(const nlohmann::json& profile : document.at("animationReferenceProfiles"))
			{
				if(profile.at("id").get<std::string>() == id)
				{
					return profile;
				}
			}
			throw TestFailure("Frozen animation profile is missing: " + std::string(id));
		}
	}

	void RegisterGpuAnimationPerformanceGateTests(TestRegistry& registry)
	{
		registry.Add("Animation.GpuGateContract.FrozenManifestHashAndCrowdProfileMatch", [](TestContext& context) {
			const std::string workloadText = ReadText(SourcePath("Tests/Tools/Animation/PerformanceWorkloads.json"));
			const nlohmann::json document = nlohmann::json::parse(workloadText);
			context.ExpectEqual(std::string(kFrozenWorkloadHash), Sha256(workloadText),
				"The source-controlled workload bytes must retain their reviewed SHA-256 identity.");
			context.Expect(kAnimationPerformanceWorkloadsSha256 == kFrozenWorkloadHash,
				"Runtime diagnostics must publish the same frozen workload identity as the gate tools.");

			const nlohmann::json& authority = document.at("authority").at("runtimeContract");
			context.Expect(authority.at("sha256").get<std::string>() == kFrozenRuntimeContractHash,
				"The workload manifest must retain the frozen runtime-contract hash.");
			context.ExpectEqual(std::string(kFrozenRuntimeContractHash),
				Sha256(ReadText(SourcePath(authority.at("path").get<std::string>()))),
				"The runtime contract bytes must match the authority recorded by the workload manifest.");

			const nlohmann::json& crowd = FindProfile(document, "crowd");
			const nlohmann::json& content = crowd.at("content");
			const nlohmann::json& acceptance = crowd.at("acceptance");
			context.Expect(content.at("activeCharacters") == 1000
				&& content.at("jointsPerCharacter") == 100
				&& content.at("blendedClips") == 2
				&& content.at("measuredSecondsAfterWarmup") == 60,
				"The crowd fixture must remain 1,000 100-joint characters with two blended clips for 60 measured seconds.");
			context.Expect(acceptance.at("minimumUpdatesPerSecond") == 60
				&& acceptance.at("maximumAnimationFrameMilliseconds") == 50
				&& acceptance.at("minimumGpuVisualCpuReductionPercent") == 75
				&& acceptance.at("maximumCompletePoseReadbacks") == 0
				&& acceptance.at("maximumAnimationGpuPoseWaits") == 0,
				"The frozen 60-Hz, 50-ms, 75-percent-offload, and zero-stall thresholds may not be weakened.");
		});

		registry.Add("Animation.GpuGateContract.NonTimedResultRequiresZeroPoseTransferAndWait", [](TestContext& context) {
			GpuGateResultV1 result;
			result.counters.activeCharacters = 1000U;
			result.counters.jointsPerCharacter = 100U;
			result.counters.blendedClipsPerCharacter = 2U;
			result.counters.cpuSampledJoints = 0U;
			result.counters.gpuDispatches = 18U;
			context.Expect(ValidateGpuGateResult(result, false),
				"ImplementationStage may prove the non-timed zero complete-pose transfer and GPU-wait invariants.");

			result.counters.completePoseReadbacks = 1U;
			context.Expect(!ValidateGpuGateResult(result, false),
				"One complete-pose readback must fail even a non-timed contract-only result.");
			result.counters.completePoseReadbacks = 0U;
			result.counters.completePoseReadbackBytes = 48U;
			context.Expect(!ValidateGpuGateResult(result, false),
				"Pose-transfer bytes must not hide behind a zero operation count.");
			result.counters.completePoseReadbackBytes = 0U;
			result.counters.animationGpuPoseWaits = 1U;
			context.Expect(!ValidateGpuGateResult(result, false),
				"Animation may not wait for ordinary GPU pose completion.");
		});

		registry.Add("Animation.GpuGateContract.TimedAcceptanceIsFinalPhaseOnly", [](TestContext& context) {
			GpuGateResultV1 result;
			result.runKind = GpuGateRunKind::TimedPhase18;
			result.timedAcceptanceAttempted = true;
			result.counters.activeCharacters = 1000U;
			result.counters.jointsPerCharacter = 100U;
			result.counters.blendedClipsPerCharacter = 2U;
			result.updatesPerSecond = 60.0;
			result.maximumAnimationFrameMilliseconds = 50.0;
			result.gpuVisualCpuReductionPercent = 75.0;
			context.Expect(!ValidateGpuGateResult(result, false),
				"A pre-final run must reject otherwise passing wall-clock measurements.");
			context.Expect(ValidateGpuGateResult(result, true),
				"Final ImplementationStage may evaluate exact-boundary measurements with the frozen schema.");

			result.gpuVisualCpuReductionPercent = std::numeric_limits<double>::quiet_NaN();
			context.Expect(!ValidateGpuGateResult(result, true),
				"A missing or non-finite performance metric must fail closed.");
		});

		registry.Add("Animation.GpuGateContract.DormantAndStaticControlsStayZeroWork", [](TestContext& context) {
			const AnimationDiagnosticsSnapshot dormant = MakeDormantAnimationDiagnostics(3U, 7, 1U);
			context.Expect(ValidateAnimationDiagnosticsSnapshot(dormant),
				"The dormant control must remain a valid copied diagnostics record.");
			context.Expect(dormant.counters.activeAnimators == 0U
				&& dormant.counters.evaluatedAnimators == 0U
				&& dormant.counters.cpuSampledJoints == 0U,
				"Dormant and static controls may not sample animation on the CPU.");
			context.Expect(dormant.counters.gpuDispatches == 0U
				&& dormant.counters.gpuUploadBytes == 0U
				&& dormant.counters.gpuReadbacks == 0U
				&& dormant.counters.gpuWaits == 0U,
				"Dormant and static controls must submit, transfer, and wait for zero animation GPU work.");
		});

		registry.Add("Animation.GpuGateContract.ActiveTransferCountersRejectCompletePoseReadback", [](TestContext& context) {
			AnimationDiagnosticsSnapshot snapshot;
			snapshot.dormant = false;
			snapshot.detailLevel = AnimationDiagnosticsDetailLevel::Summary;
			snapshot.counters.activeAnimators = 2U;
			snapshot.counters.evaluatedAnimators = 2U;
			snapshot.counters.gpuVisualAnimators = 1U;
			snapshot.counters.hybridAuthoritativeAnimators = 1U;
			snapshot.counters.cpuSampledJoints = 12U;
			snapshot.counters.gpuEvaluatedJoints = 200U;
			snapshot.counters.gpuBatches = 1U;
			snapshot.counters.gpuDispatches = 8U;
			snapshot.counters.gpuTransferBytes = 256U;
			snapshot.counters.gpuReadbacks = 1U;
			snapshot.counters.gpuReadbackBytes = 32U;
			snapshot.counters.boundedMetadataReadbackBytes = 32U;
			context.Expect(ValidateAnimationDiagnosticsSnapshot(snapshot),
				"Bounded asynchronous metadata may be reported without a full-pose transfer.");

			snapshot.counters.completePoseReadbackBytes = 48U;
			context.Expect(!ValidateAnimationDiagnosticsSnapshot(snapshot),
				"A complete-pose readback must invalidate the whole diagnostics snapshot.");
		});

		registry.Add("Animation.GpuGateContract.DetailedModeDiagnosticsCopyAtomically", [](TestContext& context) {
			ClearPublishedAnimationDetailedDiagnostics();
			AnimationDetailedDiagnosticsRequest request =
				AcquireAnimationDetailedDiagnosticsRequest();
			context.Expect(request.IsActive(),
				"The detailed diagnostics fixture must own an opt-in publication request.");
			AnimationDetailedDiagnosticsSnapshot snapshot;
			snapshot.frameIndex = 42U;
			snapshot.collapsed = false;
			AnimationAnimatorDiagnosticRecord animator;
			animator.stableInstanceId = 77U;
			animator.instanceSlot = 3U;
			animator.instanceGeneration = 4U;
			animator.requestedPolicy = AnimationExecutionPolicy::Auto;
			animator.effectiveMode = AnimationEffectiveExecutionMode::GpuVisual;
			animator.selectionReason = AnimationExecutionModeReason::GpuVisualEligible;
			animator.poseResidency = AnimationPoseResidencyState::GpuResident;
			animator.gpuStageMask = 0xffU;
			animator.batchOrdinal = 2U;
			animator.gpuEvaluatedElements = 100U;
			animator.transferBytes = 64U;
			snapshot.animators.push_back(animator);
			snapshot.modeSwitchHistory.push_back(AnimationModeSwitchDiagnosticRecord{
				77U,
				41,
				AnimationEffectiveExecutionMode::CpuDeterministicFallback,
				AnimationEffectiveExecutionMode::GpuVisual,
				AnimationExecutionModeReason::GpuVisualEligible,
				0x1234U});
			context.Expect(PublishAnimationDetailedDiagnosticsSnapshot(snapshot),
				"A complete per-animator mode and switch record should publish as one copied value.");

			snapshot.animators.front().stableInstanceId = 999U;
			const auto retained = AcquireAnimationDetailedDiagnosticsSnapshot();
			context.Expect(retained.has_value()
				&& retained->frameIndex == 42U
				&& retained->animators.size() == 1U
				&& retained->animators.front().stableInstanceId == 77U
				&& retained->modeSwitchHistory.size() == 1U,
				"Detailed publication must not retain producer-owned vector storage.");

			AnimationDetailedDiagnosticsSnapshot invalid = *retained;
			invalid.animators.front().completePoseReadbackBytes = 48U;
			context.Expect(!PublishAnimationDetailedDiagnosticsSnapshot(invalid),
				"Invalid detailed evidence must be rejected without replacing the last good copy.");
			const auto stillRetained = AcquireAnimationDetailedDiagnosticsSnapshot();
			context.Expect(stillRetained.has_value()
				&& stillRetained->animators.front().completePoseReadbackBytes == 0U,
				"Rejected detailed publication must be atomic.");
			ClearPublishedAnimationDetailedDiagnostics();
		});

		registry.Add("Animation.GpuGateContract.DetailedDiagnosticsRequestsAreReferenceCounted", [](TestContext& context) {
			ClearPublishedAnimationDetailedDiagnostics();
			AnimationDetailedDiagnosticsSnapshot snapshot;
			snapshot.frameIndex = 73U;
			snapshot.collapsed = false;
			AnimationAnimatorDiagnosticRecord record;
			record.stableInstanceId = 0x0000000100000001ULL;
			record.instanceGeneration = 1U;
			record.selectionReason = AnimationExecutionModeReason::ForceCpuRequested;
			record.fallbackReason = AnimationExecutionModeReason::ForceCpuRequested;
			record.poseResidency = AnimationPoseResidencyState::CpuOnly;
			snapshot.animators.push_back(record);

			context.Expect(!IsAnimationDetailedDiagnosticsRequested()
				&& !PublishAnimationDetailedDiagnosticsSnapshot(snapshot)
				&& !AcquireAnimationDetailedDiagnosticsSnapshot().has_value(),
				"Without an opt-in consumer, detailed publication must retain no rows.");

			AnimationDetailedDiagnosticsRequest first = AcquireAnimationDetailedDiagnosticsRequest();
			AnimationDetailedDiagnosticsRequest second = AcquireAnimationDetailedDiagnosticsRequest();
			context.Expect(first.IsActive() && second.IsActive()
				&& IsAnimationDetailedDiagnosticsRequested()
				&& PublishAnimationDetailedDiagnosticsSnapshot(snapshot),
				"Independent editor and MCP requests must jointly enable one bounded copied publication.");
			first.Reset();
			context.Expect(IsAnimationDetailedDiagnosticsRequested()
				&& AcquireAnimationDetailedDiagnosticsSnapshot().has_value(),
				"Releasing one consumer must not disable the other consumer's detailed rows.");
			second.Reset();
			context.Expect(!IsAnimationDetailedDiagnosticsRequested()
				&& !AcquireAnimationDetailedDiagnosticsSnapshot().has_value(),
				"The final release must clear retained detailed rows immediately.");
		});
	}
}
