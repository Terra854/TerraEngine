// AnimationSoakRunner.cpp - headless non-performance stability slices for animation production validation.
//
// RunAnimationSoak.ps1 launches this executable repeatedly. Each process owns a
// short, bounded slice so a timeout identifies a deadlock and a fresh process
// gives the durable driver a comparable resource snapshot without turning the
// runner itself into an unbounded long-lived diagnostic service.

#include "Animation/Determinism/AnimationHistory.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Animation/Streaming/AnimationHotReload.h"
#include "Animation/Streaming/AnimationStreaming.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#else
#include <unistd.h>
#endif

namespace
{
	using namespace TerraEngine::Animation;

	constexpr std::uint32_t kDefaultSliceMilliseconds = 1000U;
	constexpr std::uint32_t kDefaultWorkerCount = 4U;
	constexpr std::size_t kHistoryCapacityPerWorker = 32U;

	struct RunnerConfiguration
	{
		std::uint32_t sliceMilliseconds = kDefaultSliceMilliseconds;
		std::uint32_t workerCount = kDefaultWorkerCount;
	};

	struct FailureCounters
	{
		std::atomic_uint64_t stalePublications = 0U;
		std::atomic_uint64_t partialPublications = 0U;
		std::atomic_uint64_t invariantFailures = 0U;
	};

	struct ProcessResources
	{
		std::uint64_t privateBytes = 0U;
		std::uint64_t handleCount = 0U;
		std::uint64_t threadCount = 0U;
	};

	[[nodiscard]] std::string ReadEnvironment(const char* name)
	{
		if (const char* value = std::getenv(name); value != nullptr)
		{
			return value;
		}
		return {};
	}

	[[nodiscard]] bool ParseUnsigned(std::string_view text, std::uint64_t& value)
	{
		if (text.empty())
		{
			return false;
		}
		const char* begin = text.data();
		const char* end = begin + text.size();
		const auto result = std::from_chars(begin, end, value);
		return result.ec == std::errc{} && result.ptr == end;
	}

	[[nodiscard]] bool ParseArguments(int argc, char** argv, RunnerConfiguration& configuration)
	{
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument(argv[index]);
			if ((argument == "--slice-ms" || argument == "--workers") && index + 1 < argc)
			{
				std::uint64_t parsed = 0U;
				if (!ParseUnsigned(argv[++index], parsed))
				{
					return false;
				}
				if (argument == "--slice-ms")
				{
					if (parsed < 100U || parsed > 600000U)
					{
						return false;
					}
					configuration.sliceMilliseconds = static_cast<std::uint32_t>(parsed);
				}
				else
				{
					if (parsed == 0U || parsed > 32U)
					{
						return false;
					}
					configuration.workerCount = static_cast<std::uint32_t>(parsed);
				}
				continue;
			}
			return false;
		}
		return true;
	}

	[[nodiscard]] TerraEngine::UUID MakeId(std::uint32_t workerIndex, std::uint32_t role)
	{
		TerraEngine::UUID id{};
		id[0] = static_cast<std::uint8_t>((workerIndex + 1U) & 0xffU);
		id[1] = static_cast<std::uint8_t>(role & 0xffU);
		id[15] = 0x43U;
		return id;
	}

	[[nodiscard]] AnimationReloadCandidate MakeReloadCandidate(
		const TerraEngine::UUID& assetId,
		AnimationReloadAssetType assetType,
		std::uint64_t generation,
		std::uint64_t compatibilityFingerprint)
	{
		AnimationReloadCandidate candidate;
		candidate.assetId = assetId;
		candidate.assetType = assetType;
		candidate.generation = generation;
		candidate.compatibilityFingerprint = compatibilityFingerprint;
		candidate.contentFingerprint = compatibilityFingerprint ^ (generation * 0x9e3779b97f4a7c15ULL);
		candidate.parsed = true;
		candidate.dependenciesReady = true;
		return candidate;
	}

	[[nodiscard]] bool ExerciseStreaming(std::uint32_t workerIndex, std::uint64_t cycle)
	{
		AnimationStreamingCoordinator coordinator(2U, 4U, 1024U, 16U);
		AnimationStreamRequest request;
		request.assetId = MakeId(workerIndex, 10U);
		request.kind = (cycle & 1U) == 0U
			? AnimationStreamPayloadKind::ClipSegment
			: AnimationStreamPayloadKind::MotionDatabaseBlock;
		request.blockIndex = static_cast<std::uint32_t>(cycle & 3U);
		request.byteSize = 64U;
		request.generation = 1U;
		request.gameplayCritical = (cycle % 7U) == 0U;

		const AnimationStreamDecision first = coordinator.Request(request);
		if (!first.accepted || !first.useSafeFallback || !coordinator.Complete(request, true))
		{
			return false;
		}

		request.generation = 2U;
		const AnimationStreamDecision replacement = coordinator.Request(request);
		if (!replacement.accepted || replacement.fallbackGeneration != 1U
			|| replacement.fallbackKind != AnimationStreamFallbackKind::LastResidentGeneration)
		{
			return false;
		}

		// Cancellation must win over a worker that completes after its generation
		// was invalidated. The old resident generation remains the safe authority.
		if (!coordinator.Cancel(request) || coordinator.Complete(request, true))
		{
			return false;
		}
		const auto cancelled = coordinator.Query(request);
		return cancelled.has_value() && cancelled->state == AnimationStreamState::Cancelled
			&& cancelled->fallbackGeneration == 1U && cancelled->useSafeFallback;
	}

	[[nodiscard]] bool ExerciseHotReload(std::uint32_t workerIndex)
	{
		AnimationHotReloadPublisher publisher(8U, 8U, 8U, 8U);
		const TerraEngine::UUID skeletonId = MakeId(workerIndex, 20U);
		const TerraEngine::UUID clipId = MakeId(workerIndex, 21U);
		const TerraEngine::UUID instanceId = MakeId(workerIndex, 22U);
		const std::uint64_t skeletonCompatibility = 0x1000U + workerIndex;
		const std::uint64_t clipCompatibility = 0x2000U + workerIndex;

		AnimationReloadCandidate skeleton = MakeReloadCandidate(
			skeletonId, AnimationReloadAssetType::Skeleton, 1U, skeletonCompatibility);
		AnimationReloadCandidate clip = MakeReloadCandidate(
			clipId, AnimationReloadAssetType::AnimationClip, 1U, clipCompatibility);
		clip.dependencies.push_back({skeletonId, 1U, skeletonCompatibility, true});
		const std::array initial{std::move(skeleton), std::move(clip)};
		const auto initialOutcomes = publisher.PublishBatch(initial, 1U);
		if (initialOutcomes.size() != 2U || !initialOutcomes[0].published
			|| !initialOutcomes[1].published || !publisher.RegisterDependency(clipId, skeletonId)
			|| !publisher.BindInstance(instanceId, clipId, 1U))
		{
			return false;
		}

		AnimationReloadCandidate corrupt = MakeReloadCandidate(
			skeletonId, AnimationReloadAssetType::Skeleton, 2U, skeletonCompatibility);
		corrupt.parsed = false;
		const auto rejected = publisher.PublishBatch(std::array{corrupt}, 2U);
		if (rejected.empty() || rejected.front().published
			|| publisher.Query(skeletonId).activeGeneration != 1U)
		{
			return false;
		}

		AnimationReloadCandidate skeleton2 = MakeReloadCandidate(
			skeletonId, AnimationReloadAssetType::Skeleton, 2U, skeletonCompatibility);
		AnimationReloadCandidate clip2 = MakeReloadCandidate(
			clipId, AnimationReloadAssetType::AnimationClip, 2U, clipCompatibility);
		clip2.dependencies.push_back({skeletonId, 2U, skeletonCompatibility, true});
		const std::array replacement{std::move(skeleton2), std::move(clip2)};
		const auto outcomes = publisher.PublishBatch(replacement, 2U);
		const auto instance = publisher.QueryInstance(instanceId);
		return outcomes.size() == 2U && outcomes[0].published && outcomes[1].published
			&& publisher.Query(skeletonId).activeGeneration == 2U
			&& publisher.Query(clipId).activeGeneration == 2U
			&& instance.has_value() && instance->generation == 2U
			&& instance->switchBoundary == 2U
			&& publisher.RetireThroughBoundary(2U) == 2U;
	}

	[[nodiscard]] bool StoreHistorySample(
		AnimationHistory& history,
		std::uint32_t workerIndex,
		std::uint32_t workerCount,
		std::uint64_t cycle)
	{
		AnimationRollbackSnapshot source;
		source.animator = {workerIndex + 1U, 1U};
		source.tick = static_cast<AnimationFixedTick>(cycle);
		source.publicationGeneration = 1U;
		source.frozenWorkerCount = workerCount;
		source.fingerprint = {1U + workerIndex, 2U, 3U, 4U, 5U, 6U, 7U};
		source.generations = {1U, 1U, 1U, 1U, 1U, 1U, 1U};
		source.state.push_back({
			static_cast<AnimationDeterministicStateKind>(
				1U + static_cast<std::uint16_t>(cycle % 15U)),
			0U,
			0U,
			100U + (cycle % 15U),
			{cycle, cycle ^ 0x55aa55aaU, workerIndex, 43U}});

		AnimationRollbackSnapshot captured;
		return AnimationRollbackSnapshotCodec{}.Capture(source, captured) == AnimationStatus::Ok
			&& history.Store(std::move(captured)) == AnimationStatus::Ok;
	}

	void RunWorker(
		std::uint32_t workerIndex,
		std::uint32_t workerCount,
		std::chrono::steady_clock::time_point deadline,
		AnimatorInstanceStore& instanceStore,
		AnimationHistory& history,
		FailureCounters& failures,
		std::atomic_uint64_t& completedCycles,
		std::vector<std::uint64_t>& workerCycles)
	{
		std::uint64_t cycle = 0U;
		do
		{
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId = MakeId(workerIndex, 1U);
			descriptor.componentGeneration = 1U;
			const AnimatorInstanceHandle handle = instanceStore.Create(descriptor);
			if (!handle.IsValid() || !instanceStore.IsAlive(handle)
				|| !instanceStore.Invalidate(handle) || instanceStore.IsAlive(handle))
			{
				failures.invariantFailures.fetch_add(1U, std::memory_order_relaxed);
				break;
			}

			if (!ExerciseStreaming(workerIndex, cycle))
			{
				failures.stalePublications.fetch_add(1U, std::memory_order_relaxed);
				break;
			}
			if (!ExerciseHotReload(workerIndex))
			{
				failures.partialPublications.fetch_add(1U, std::memory_order_relaxed);
				break;
			}
			if (!StoreHistorySample(history, workerIndex, workerCount, cycle))
			{
				failures.invariantFailures.fetch_add(1U, std::memory_order_relaxed);
				break;
			}
			++cycle;
			completedCycles.fetch_add(1U, std::memory_order_relaxed);
		} while (std::chrono::steady_clock::now() < deadline);
		workerCycles[workerIndex] = cycle;
	}

	[[nodiscard]] std::string EscapeJson(std::string_view value)
	{
		std::string escaped;
		escaped.reserve(value.size());
		for (const char character : value)
		{
			switch (character)
			{
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default: escaped += character; break;
			}
		}
		return escaped;
	}

	[[nodiscard]] ProcessResources ReadProcessResources()
	{
		ProcessResources resources;
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX memory{};
		memory.cb = sizeof(memory);
		if (GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE)
		{
			resources.privateBytes = static_cast<std::uint64_t>(memory.PrivateUsage);
		}
		DWORD handleCount = 0U;
		if (GetProcessHandleCount(GetCurrentProcess(), &handleCount) != FALSE)
		{
			resources.handleCount = handleCount;
		}
		const DWORD processId = GetCurrentProcessId();
		if (const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0U);
			snapshot != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);
			if (Thread32First(snapshot, &entry) != FALSE)
			{
				do
				{
					resources.threadCount += entry.th32OwnerProcessID == processId ? 1U : 0U;
				} while (Thread32Next(snapshot, &entry) != FALSE);
			}
			CloseHandle(snapshot);
		}
#else
		std::ifstream status("/proc/self/status");
		std::string key;
		while (status >> key)
		{
			if (key == "VmRSS:")
			{
				std::uint64_t kibibytes = 0U;
				status >> kibibytes;
				resources.privateBytes = kibibytes * 1024U;
			}
			else if (key == "Threads:")
			{
				status >> resources.threadCount;
			}
			status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		}
		std::error_code error;
		for (std::filesystem::directory_iterator iterator("/proc/self/fd", error), end;
			!error && iterator != end; iterator.increment(error))
		{
			++resources.handleCount;
		}
#endif
		return resources;
	}

	[[nodiscard]] bool WriteResultAtomically(
		const std::filesystem::path& resultPath,
		std::string_view json)
	{
		std::error_code error;
		std::filesystem::create_directories(resultPath.parent_path(), error);
		if (error)
		{
			return false;
		}
		const std::filesystem::path temporary = resultPath.string() + ".pending";
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output)
			{
				return false;
			}
			output.write(json.data(), static_cast<std::streamsize>(json.size()));
			output.put('\n');
			output.flush();
			if (!output)
			{
				return false;
			}
		}
		std::filesystem::rename(temporary, resultPath, error);
		if (error)
		{
			std::filesystem::remove(temporary, error);
			return false;
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	RunnerConfiguration configuration;
	if (!ParseArguments(argc, argv, configuration))
	{
		std::cerr << "Usage: TerraEngineAnimationSoakRunner [--slice-ms 100..600000] [--workers 1..32]\n";
		return 2;
	}

	const std::string runId = ReadEnvironment("TERRAENGINE_ANIMATION_SOAK_RUN_ID");
	const std::string iterationText = ReadEnvironment("TERRAENGINE_ANIMATION_SOAK_ITERATION");
	const std::string resultText = ReadEnvironment("TERRAENGINE_ANIMATION_SOAK_RESULT");
	std::uint64_t iteration = 0U;
	if (runId.empty() || resultText.empty() || !ParseUnsigned(iterationText, iteration))
	{
		std::cerr << "The soak driver environment is incomplete or invalid.\n";
		return 2;
	}

	FailureCounters failures;
	std::atomic_uint64_t completedCycles = 0U;
	AnimatorInstanceStore instanceStore;
	AnimationHistory history({kHistoryCapacityPerWorker, AnimationHistoryPressurePolicy::EvictOldest});
	std::vector<std::uint64_t> workerCycles(configuration.workerCount, 0U);
	std::vector<std::thread> workers;
	workers.reserve(configuration.workerCount);
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(configuration.sliceMilliseconds);
	for (std::uint32_t workerIndex = 0U; workerIndex < configuration.workerCount; ++workerIndex)
	{
		workers.emplace_back(RunWorker, workerIndex, configuration.workerCount, deadline,
			std::ref(instanceStore), std::ref(history), std::ref(failures),
			std::ref(completedCycles), std::ref(workerCycles));
	}
	for (std::thread& worker : workers)
	{
		worker.join();
	}

	if (instanceStore.ActiveCount() != 0U || completedCycles.load(std::memory_order_relaxed) == 0U)
	{
		failures.invariantFailures.fetch_add(1U, std::memory_order_relaxed);
	}
	std::uint64_t retainedHistoryRecords = 0U;
	for (const std::uint64_t cycles : workerCycles)
	{
		retainedHistoryRecords += std::min<std::uint64_t>(cycles, kHistoryCapacityPerWorker);
	}

	const std::uint64_t stalePublications = failures.stalePublications.load(std::memory_order_relaxed);
	const std::uint64_t partialPublications = failures.partialPublications.load(std::memory_order_relaxed)
		+ failures.invariantFailures.load(std::memory_order_relaxed);
	const bool passed = stalePublications == 0U && partialPublications == 0U;
	const ProcessResources resources = ReadProcessResources();
	const std::uint64_t heartbeat = iteration == std::numeric_limits<std::uint64_t>::max()
		? iteration : iteration + 1U;

	// This headless correctness runner does not initialize Vulkan, so reporting
	// zero GPU residency is an observed ownership fact, not a performance claim.
	const std::string json = "{\"schemaVersion\":1,\"runId\":\"" + EscapeJson(runId)
		+ "\",\"iteration\":" + std::to_string(iteration)
		+ ",\"passed\":" + (passed ? "true" : "false")
		+ ",\"heartbeatSequence\":" + std::to_string(heartbeat)
		+ ",\"counters\":{\"deadlocks\":0,\"crashes\":0,\"stalePublications\":"
		+ std::to_string(stalePublications)
		+ ",\"partialPublications\":" + std::to_string(partialPublications)
		+ ",\"unrecoverableRendererFailures\":0,\"unrecoverablePhysicsFailures\":0}"
		+ ",\"resources\":{\"privateBytes\":" + std::to_string(resources.privateBytes)
		+ ",\"gpuResidentBytes\":0,\"handleCount\":" + std::to_string(resources.handleCount)
		+ ",\"threadCount\":" + std::to_string(resources.threadCount)
		+ ",\"retainedHistoryRecords\":" + std::to_string(retainedHistoryRecords) + "}}";
	if (!WriteResultAtomically(std::filesystem::path(resultText), json))
	{
		std::cerr << "Could not atomically publish the soak iteration result.\n";
		return 3;
	}
	return passed ? 0 : 1;
}
