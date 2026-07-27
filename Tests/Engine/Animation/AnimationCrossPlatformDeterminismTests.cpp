// AnimationCrossPlatformDeterminismTests.cpp - Portable fixed-tick checksum contracts.

#include "TestHarness.h"

#include "Animation/Determinism/AnimationRollbackSnapshot.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		// These names deliberately include the installed platform/toolchain contract.
		// The test rehearses each lane in the current process; T270 remains responsible
		// for launching the real binaries and comparing 100,000-tick evidence.
		enum class FrozenLaneKind : std::uint8_t {
			WindowsMsvcInstalledRuntime = 0U,
			WindowsMsvcInstalledHeadless = 1U,
			Ubuntu2404Gcc14InstalledHeadless = 2U
		};

		struct FrozenLane {
			FrozenLaneKind kind = FrozenLaneKind::WindowsMsvcInstalledRuntime;
			std::array<std::uint32_t, 4U> workerCounts{};
		};

		// Zero is a symbolic rule, never a worker count: resolve it from the Ubuntu
		// runner as maximum available minus one when that lane actually executes.
		constexpr std::uint32_t kMaximumAvailableMinusOne = 0U;
		constexpr std::array<std::uint32_t, 4U> kWindowsWorkers{1U, 4U, 16U, 31U};
		constexpr std::array<std::uint32_t, 4U> kUbuntuWorkers{
			1U, 4U, 16U, kMaximumAvailableMinusOne};
		constexpr std::array<FrozenLane, 3U> kFrozenLanes{{
			{FrozenLaneKind::WindowsMsvcInstalledRuntime, kWindowsWorkers},
			{FrozenLaneKind::WindowsMsvcInstalledHeadless, kWindowsWorkers},
			{FrozenLaneKind::Ubuntu2404Gcc14InstalledHeadless, kUbuntuWorkers}
		}};

		// Ordinary CTest runs keep a short contract rehearsal. T270 selects the
		// frozen 100,000-tick acceptance profile explicitly so every real compiler
		// lane performs the complete correctness workload without making unrelated
		// local test iterations pay that cost.
		constexpr std::size_t kSmokeFixedTickCount = 257U;
		constexpr std::size_t kAcceptanceFixedTickCount = 100000U;
		constexpr std::size_t kSemanticHashCount = 8U;
		constexpr std::uint64_t kHashOffset = 1469598103934665603ULL;
		constexpr std::uint64_t kHashPrime = 1099511628211ULL;
		// This value is filled from the production codec after the fixture is first
		// compiled. Keeping one source-controlled value lets MSVC and GCC compare to
		// the same answer instead of merely agreeing with themselves.
		constexpr std::uint64_t kExpectedFixedTickStreamHash = 1517678832756790257ULL;
		// Filled from the Windows MSVC reference after the complete stream is first
		// executed. Linux/GCC must compare against this source-controlled value,
		// rather than accepting a lane-specific answer merely because it is stable.
		constexpr std::uint64_t kExpectedAcceptanceStreamHash = 17254665474189528098ULL;

		struct FixedTickProfile {
			std::size_t tickCount = kSmokeFixedTickCount;
			std::uint64_t expectedHash = kExpectedFixedTickStreamHash;
			bool valid = true;
		};

		FixedTickProfile ResolveFixedTickProfile() noexcept
		{
			const char* rawValue = std::getenv("TERRAENGINE_ANIMATION_DETERMINISM_TICKS");
			if(rawValue == nullptr || std::string_view(rawValue).empty()
				|| std::string_view(rawValue) == "257") {
				return {};
			}
			if(std::string_view(rawValue) == "100000") {
				return {kAcceptanceFixedTickCount, kExpectedAcceptanceStreamHash, true};
			}
			// Fail closed instead of silently shortening an acceptance invocation due
			// to a misspelled environment value.
			return {0U, 0U, false};
		}

		enum class SemanticHash : std::size_t {
			Controller = 0U,
			Event = 1U,
			RootMotion = 2U,
			Ik = 3U,
			Retarget = 4U,
			MotionMatch = 5U,
			Morph = 6U,
			Ragdoll = 7U
		};

		struct FixedTickHashes {
			std::array<std::uint64_t, kSemanticHashCount> semantic{};
			std::uint64_t state = 0U;
			std::uint64_t pose = 0U;
			auto operator<=>(const FixedTickHashes&) const = default;
		};

		struct FixedTickStream {
			AnimationStatus status = AnimationStatus::NotReady;
			std::vector<FixedTickHashes> ticks;
			std::uint64_t aggregateHash = 0U;
		};

		std::uint32_t ResolveWorkerCount(std::uint32_t value) noexcept
		{
			if(value != kMaximumAvailableMinusOne) return value;
			const std::uint32_t available = std::thread::hardware_concurrency();
			return available > 1U ? available - 1U : 1U;
		}

		bool IsHeadless(FrozenLaneKind lane) noexcept
		{
			return lane != FrozenLaneKind::WindowsMsvcInstalledRuntime;
		}

		bool IsUbuntu(FrozenLaneKind lane) noexcept
		{
			return lane == FrozenLaneKind::Ubuntu2404Gcc14InstalledHeadless;
		}

		bool RunsInCurrentAcceptanceProcess(FrozenLaneKind lane) noexcept
		{
			// The short developer profile rehearses every partition in one process.
			// The T270 acceptance profile must instead report only lanes executed by
			// the real compiled platform so its evidence cannot imply that MSVC ran on
			// Linux, or that GCC ran on Windows.
#if defined(_WIN32)
			return lane != FrozenLaneKind::Ubuntu2404Gcc14InstalledHeadless;
#else
			return lane == FrozenLaneKind::Ubuntu2404Gcc14InstalledHeadless;
#endif
		}

		AnimationGuid MakeGuid(std::uint8_t seed) noexcept
		{
			AnimationGuid value;
			value.value[0U] = seed;
			value.value[15U] = static_cast<std::uint8_t>(seed ^ 0x5aU);
			return value;
		}

		std::uint32_t ExactFractionBits(AnimationFixedTick tick, std::uint32_t phase = 0U) noexcept
		{
			// Denominators that are powers of two are represented exactly in binary32,
			// so the fixture introduces no compiler-specific decimal rounding.
			const std::uint32_t numerator = (static_cast<std::uint32_t>(tick) + phase) & 15U;
			return EncodeAnimationNetworkFloat(static_cast<float>(numerator) * 0.0625F);
		}

		AnimationNetworkFloat3Bits VectorBits(
			AnimationFixedTick tick, std::uint32_t phase = 0U) noexcept
		{
			return {{ExactFractionBits(tick, phase), ExactFractionBits(tick, phase + 3U),
				ExactFractionBits(tick, phase + 7U)}};
		}

		AnimationNetworkTransformBits TransformBits(AnimationFixedTick tick) noexcept
		{
			AnimationNetworkTransformBits value;
			value.translation = VectorBits(tick);
			value.rotation.words = {0U, 0U, 0U, EncodeAnimationNetworkFloat(1.0F)};
			value.scale.words = {EncodeAnimationNetworkFloat(1.0F),
				EncodeAnimationNetworkFloat(1.0F), EncodeAnimationNetworkFloat(1.0F)};
			return value;
		}

		AnimationRollbackSnapshot MakeFixedTickSnapshot(
			AnimatorInstanceHandle animator,
			AnimationFixedTick tick,
			std::uint32_t workerCount)
		{
			AnimationRollbackSnapshot value;
			value.animator = animator;
			value.tick = tick;
			value.publicationGeneration = 3U;
			value.frozenWorkerCount = workerCount;
			value.fingerprint = {0x101U, 0x201U, 0x301U, 0x401U, 0x501U, 0x601U, 0x701U};
			value.generations = {1U, 2U, 3U, 4U, 5U, 6U, 7U};

			AnimationControllerRollbackState controller;
			controller.controllerInstanceId = 1001U;
			controller.activeStateId = 2001U + static_cast<std::uint64_t>(tick % 3);
			controller.previousStateId = 2001U + static_cast<std::uint64_t>((tick + 2) % 3);
			controller.stateEnteredTick = tick - (tick % 4);
			controller.lastEvaluatedTick = tick;
			controller.syncGroupId = 3001U;
			controller.syncMarkerId = 3002U + static_cast<std::uint64_t>(tick % 2);
			controller.deterministicRandomState = 0x9e3779b97f4a7c15ULL
				^ static_cast<std::uint64_t>(tick);
			value.controllers.push_back(controller);

			AnimationEventCursorRollbackState event;
			event.streamId = 4001U;
			event.stateInstanceId = 2001U;
			event.clipAssetId = MakeGuid(11U);
			event.eventId = 4100U + static_cast<std::uint64_t>(tick % 5);
			event.sourceTick = tick;
			event.loopCount = tick / 60;
			event.deliveredSequence = static_cast<std::uint64_t>(tick);
			event.deliveredThroughTick = tick;
			event.delivered = true;
			value.events.push_back(event);
			value.eventWatermark = {event.streamId, event.deliveredSequence, tick};

			AnimationRootMotionRollbackState root;
			root.accumulationId = 5001U;
			root.sourceBeginTick = tick - 1;
			root.sourceEndTick = tick;
			root.localTranslation = VectorBits(tick, 1U);
			root.worldTranslation = VectorBits(tick, 2U);
			root.localYawBits = ExactFractionBits(tick, 4U);
			root.worldYawBits = ExactFractionBits(tick, 5U);
			root.accumulatedCycles = tick / 60;
			root.sourceStateId = controller.activeStateId;
			root.layerIndex = 0U;
			root.generation = 1U;
			root.correlationId = 5100U + static_cast<std::uint64_t>(tick);
			root.appliedThroughTick = tick;
			value.rootMotionStates.push_back(root);
			value.rootMotion = {tick,
				{root.localTranslation.words[0U], root.localTranslation.words[1U],
					root.localTranslation.words[2U], root.localYawBits},
				root.correlationId, false, false};

			AnimationIkRollbackState ik;
			ik.stableNodeId = 6001U;
			ik.goalId = 6002U;
			ik.targetStableId = 6003U;
			ik.evaluatedTick = tick;
			ik.solverState = AnimationRollbackIkState::Solved;
			ik.pelvisOffsetBits = ExactFractionBits(tick, 6U);
			ik.weightBits = EncodeAnimationNetworkFloat(1.0F);
			ik.errorBits = ExactFractionBits(tick, 7U);
			ik.plantedContactId = 6004U;
			ik.lockedContactTransform = TransformBits(tick);
			ik.contactSurfaceNormal.words = {0U, EncodeAnimationNetworkFloat(1.0F), 0U};
			ik.contactContributionBits = EncodeAnimationNetworkFloat(1.0F);
			ik.contactPhase = AnimationRollbackFootContactPhase::Locked;
			value.ikStates.push_back(ik);

			AnimationRetargetRollbackState retarget;
			retarget.stableNodeId = 7001U;
			retarget.profileFingerprint = 0x7002U;
			retarget.profileGeneration = 1U;
			retarget.sourceSkeletonGeneration = 2U;
			retarget.targetSkeletonGeneration = 3U;
			retarget.sourcePoseGeneration = 4U;
			retarget.targetPoseGeneration = 5U;
			retarget.sourceTick = tick;
			retarget.normalizedTimeBits = ExactFractionBits(tick, 8U);
			retarget.deterministicHash = 0x7000000000000000ULL
				| static_cast<std::uint64_t>(tick);
			value.retargetStates.push_back(retarget);

			AnimationMotionMatchRollbackState motionMatch;
			motionMatch.stableNodeId = 8001U;
			motionMatch.databaseFingerprint = 0x8002U;
			motionMatch.databaseGeneration = 1U;
			motionMatch.querySequence = static_cast<std::uint64_t>(tick);
			motionMatch.candidateId = 8100U + static_cast<std::uint64_t>(tick % 13);
			motionMatch.sourceClipFingerprint = 0x8003U;
			motionMatch.sampleTick = tick * 2;
			motionMatch.totalScoreBits = ExactFractionBits(tick, 9U);
			motionMatch.selectionHash = 0x8000000000000000ULL
				| static_cast<std::uint64_t>(tick);
			value.motionMatchStates.push_back(motionMatch);

			value.morphStates.push_back({9001U, 9002U, ExactFractionBits(tick, 10U), 1U});

			AnimationRagdollRollbackState ragdoll;
			ragdoll.ragdollInstanceId = 10001U;
			ragdoll.bodyId = 10002U;
			ragdoll.stableJointId = 10003U;
			ragdoll.lifecycle = AnimationRollbackRagdollState::Active;
			ragdoll.worldGeneration = 1U;
			ragdoll.profileGeneration = 2U;
			ragdoll.skeletonGeneration = 3U;
			ragdoll.physicsPublicationGeneration = 4U;
			ragdoll.bodyGeneration = 5U;
			ragdoll.fixedStep = tick;
			ragdoll.modelTransform = TransformBits(tick);
			ragdoll.linearVelocity = VectorBits(tick, 11U);
			ragdoll.angularVelocity = VectorBits(tick, 12U);
			ragdoll.blendWeightBits = ExactFractionBits(tick, 13U);
			ragdoll.eventSequence = static_cast<std::uint64_t>(tick);
			ragdoll.sleeping = (tick % 2) == 0;
			value.ragdollStates.push_back(ragdoll);
			return value;
		}

		std::uint64_t ComputeSemanticHash(
			const AnimationRollbackSnapshot& captured,
			SemanticHash selected)
		{
			// Hash one typed domain at a time through the production checksum API.
			// Clearing unrelated sections makes missing coverage visible instead of
			// letting the aggregate pose checksum hide which semantic domain changed.
			AnimationRollbackSnapshot domain = captured;
			domain.controllers.clear();
			domain.events.clear();
			domain.rootMotionStates.clear();
			domain.ikStates.clear();
			domain.retargetStates.clear();
			domain.motionMatchStates.clear();
			domain.morphStates.clear();
			domain.ragdollStates.clear();
			domain.eventWatermark = {};
			domain.rootMotion = {};
			domain.state.clear();
			switch(selected) {
			case SemanticHash::Controller:
				domain.controllers = captured.controllers;
				break;
			case SemanticHash::Event:
				domain.events = captured.events;
				domain.eventWatermark = captured.eventWatermark;
				break;
			case SemanticHash::RootMotion:
				domain.rootMotionStates = captured.rootMotionStates;
				domain.rootMotion = captured.rootMotion;
				break;
			case SemanticHash::Ik:
				domain.ikStates = captured.ikStates;
				break;
			case SemanticHash::Retarget:
				domain.retargetStates = captured.retargetStates;
				break;
			case SemanticHash::MotionMatch:
				domain.motionMatchStates = captured.motionMatchStates;
				break;
			case SemanticHash::Morph:
				domain.morphStates = captured.morphStates;
				break;
			case SemanticHash::Ragdoll:
				domain.ragdollStates = captured.ragdollStates;
				break;
			}
			return ComputeDeterministicStateChecksum(domain);
		}

		FixedTickHashes CaptureHashes(
			AnimatorInstanceHandle animator,
			AnimationFixedTick tick,
			std::uint32_t workerCount,
			AnimationStatus& status)
		{
			AnimationRollbackSnapshot captured;
			status = AnimationRollbackSnapshotCodec{}.Capture(
				MakeFixedTickSnapshot(animator, tick, workerCount), captured);
			FixedTickHashes hashes;
			if(status != AnimationStatus::Ok) return hashes;
			for(std::size_t index = 0U; index < kSemanticHashCount; ++index) {
				hashes.semantic[index] = ComputeSemanticHash(
					captured, static_cast<SemanticHash>(index));
			}
			hashes.state = captured.stateChecksum;
			hashes.pose = captured.poseChecksum;
			return hashes;
		}

		std::uint64_t MixHash(std::uint64_t hash, std::uint64_t value) noexcept
		{
			return (hash ^ value) * kHashPrime;
		}

		std::uint64_t AggregateStream(const std::vector<FixedTickHashes>& ticks) noexcept
		{
			std::uint64_t hash = kHashOffset;
			for(std::size_t tickIndex = 0U; tickIndex < ticks.size(); ++tickIndex) {
				hash = MixHash(hash, tickIndex + 1U);
				for(const std::uint64_t semantic : ticks[tickIndex].semantic) {
					hash = MixHash(hash, semantic);
				}
				hash = MixHash(hash, ticks[tickIndex].state);
				hash = MixHash(hash, ticks[tickIndex].pose);
			}
			return hash;
		}

		FixedTickStream EvaluateLaneEquivalentStream(
			const FrozenLane& lane,
			std::uint32_t workerCount,
			std::size_t tickCount)
		{
			FixedTickStream result;
			result.ticks.resize(tickCount);
			const std::size_t partitionCount = std::min<std::size_t>(workerCount, tickCount);
			for(std::size_t ordinal = 0U; ordinal < partitionCount; ++ordinal) {
				std::size_t worker = IsHeadless(lane.kind)
					? partitionCount - ordinal - 1U
					: ordinal;
				if(IsUbuntu(lane.kind) && partitionCount > 1U) {
					// Rotate only the simulated completion order. This exercises a distinct
					// lane partition without pretending the current process runs Ubuntu.
					worker = (worker + 1U) % partitionCount;
				}
				for(std::size_t tickIndex = worker;
					tickIndex < tickCount;
					tickIndex += workerCount) {
					AnimationStatus captureStatus = AnimationStatus::NotReady;
					result.ticks[tickIndex] = CaptureHashes(
						{static_cast<std::uint32_t>(tickIndex + 1U), 1U},
						static_cast<AnimationFixedTick>(tickIndex + 1U),
						workerCount,
						captureStatus);
					if(captureStatus != AnimationStatus::Ok) {
						result.status = captureStatus;
						return result;
					}
				}
			}
			result.status = AnimationStatus::Ok;
			result.aggregateHash = AggregateStream(result.ticks);
			return result;
		}
	}

	void RegisterAnimationCrossPlatformDeterminismTests(TestRegistry& registry)
	{
		registry.Add("Animation.CrossPlatformDeterminism.FrozenLaneProfileMatchesContract", [](TestContext& context) {
			context.Expect(kFrozenLanes[0U].kind == FrozenLaneKind::WindowsMsvcInstalledRuntime
				&& kFrozenLanes[0U].workerCounts == kWindowsWorkers,
				"The Windows installed-runtime lane must freeze MSVC workers at 1/4/16/31.");
			context.Expect(kFrozenLanes[1U].kind == FrozenLaneKind::WindowsMsvcInstalledHeadless
				&& kFrozenLanes[1U].workerCounts == kWindowsWorkers,
				"The Windows installed-headless lane must use the same installed MSVC artifacts and workers.");
			context.Expect(kFrozenLanes[2U].kind == FrozenLaneKind::Ubuntu2404Gcc14InstalledHeadless
				&& kFrozenLanes[2U].workerCounts == kUbuntuWorkers
				&& ResolveWorkerCount(kFrozenLanes[2U].workerCounts.back()) > 0U,
				"The Ubuntu 24.04 GCC 14 headless lane must freeze 1/4/16 plus maximum-available-minus-one.");
		});

		registry.Add("Animation.CrossPlatformDeterminism.FixedTickTypedHashesMatchGoldenAcrossLanePartitions", [](TestContext& context) {
			const FixedTickProfile profile = ResolveFixedTickProfile();
			context.Expect(profile.valid,
				"TERRAENGINE_ANIMATION_DETERMINISM_TICKS must be unset, 257, or the frozen acceptance value 100000.");
			const FixedTickStream reference = EvaluateLaneEquivalentStream(
				kFrozenLanes[0U], ResolveWorkerCount(kFrozenLanes[0U].workerCounts[0U]),
				profile.tickCount);
			context.Expect(reference.status == AnimationStatus::Ok
				&& reference.ticks.size() == profile.tickCount,
				"The one-worker installed-runtime reference must capture every fixed tick through the production codec.");
			context.ExpectEqual(profile.expectedHash, reference.aggregateHash,
				"The compiler-neutral controller/event/root-motion/IK/retarget/MM/morph/ragdoll and pose golden changed");
			std::cout << "{\"schemaVersion\":1,\"profile\":\"cross-platform-determinism\",\"ticks\":"
				<< profile.tickCount << ",\"lane\":\"compiler-local-golden-reference\",\"workers\":1,\"aggregateHash\":\""
				<< reference.aggregateHash << "\"}\n";

			for(const FrozenLane& lane : kFrozenLanes) {
				if(profile.tickCount == kAcceptanceFixedTickCount
					&& !RunsInCurrentAcceptanceProcess(lane.kind)) {
					continue;
				}
				for(const std::uint32_t workerRule : lane.workerCounts) {
					const std::uint32_t workerCount = ResolveWorkerCount(workerRule);
					const FixedTickStream candidate = EvaluateLaneEquivalentStream(
						lane, workerCount, profile.tickCount);
					context.Expect(candidate.status == AnimationStatus::Ok
						&& candidate.ticks == reference.ticks
						&& candidate.aggregateHash == reference.aggregateHash,
						"Lane metadata, headless completion order, and frozen worker partitioning must not change any typed semantic or pose hash.");
					const char* laneName = lane.kind == FrozenLaneKind::WindowsMsvcInstalledRuntime
						? "windows-msvc-installed-runtime"
						: lane.kind == FrozenLaneKind::WindowsMsvcInstalledHeadless
							? "windows-msvc-installed-headless"
							: "ubuntu-24.04-gcc14-installed-headless";
					std::cout << "{\"schemaVersion\":1,\"profile\":\"cross-platform-determinism\",\"ticks\":"
						<< profile.tickCount << ",\"lane\":\"" << laneName
						<< "\",\"workers\":" << workerCount << ",\"aggregateHash\":\""
						<< candidate.aggregateHash << "\"}\n";
				}
			}
		});
	}
}
