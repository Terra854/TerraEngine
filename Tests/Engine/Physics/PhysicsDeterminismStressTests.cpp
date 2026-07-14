// PhysicsDeterminismStressTests.cpp - seeded Jolt 5.6 stress replay coverage.
//
// The parent test re-executes this test binary once for each worker setting.
// That gives the replay a genuinely fresh process, backend, worker pool, and
// static state instead of assuming a freshly constructed backend is equivalent.

#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Windows.h>
#endif

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		constexpr std::uint32_t kStressBodyCount = 10000U;
		constexpr std::uint32_t kStressActiveBodyCount = 1000U;
		constexpr std::uint32_t kStressRayCount = 10000U;
		constexpr std::uint32_t kStressRayRepetitions = 100U;
		constexpr std::uint32_t kStressSeed = 137903421U;
		constexpr wchar_t kChildReplayEnvironment[] = L"TERRAENGINE_PHYSICS_STRESS_CHILD_REPLAY";
		constexpr wchar_t kTestFilterEnvironment[] = L"TERRAENGINE_TEST_FILTER";
		constexpr wchar_t kChildReplayFilter[] = L"Physics.DeterminismStress.ChildProcessReplay";

		// Jolt jobs must use the engine pool. This guard makes each replay isolated
		// and prevents work from one worker-count run leaking into the next one.
		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		struct ReplayHashes {
			std::uint64_t backendState = 0U;
			std::uint64_t sortedPackets = 0U;
			std::uint64_t warmPeakAllocations = 0U;
			std::uint64_t finalPeakAllocations = 0U;
		};

		struct ChildReplayRequest {
			// The parent passes the requested worker count and a private temporary
			// result file through environment variables, never through provider state.
			std::uint32_t workers = 0U;
			std::filesystem::path resultPath;
		};

		void HashWord(std::uint64_t& hash, std::uint64_t value) {
			// FNV-1a is sufficient here because this is a reproducibility fingerprint,
			// not a security primitive. Fixed-width input keeps it process-compatible.
			hash ^= value;
			hash *= 1099511628211ULL;
		}

		void HashFloat(std::uint64_t& hash, float value) {
			HashWord(hash, std::bit_cast<std::uint32_t>(value));
		}

		TerraEngine::UUID StableUuid(std::uint32_t index) {
			TerraEngine::UUID result{};
			for(std::size_t byte = 0U; byte < result.size(); ++byte) {
				result[byte] = static_cast<std::uint8_t>((kStressSeed + index * 37U + static_cast<std::uint32_t>(byte) * 19U) >> ((byte % 4U) * 8U));
			}
			return result;
		}

		struct StressBackend final {
			ThreadPoolSession pool;
			JoltPhysicsBackend backend;
			PhysicsWorldHandle world{{9301U}, 1U};
			PhysicsShapeHandle shape{{9301U}, 1U, 0U, 1U};

			explicit StressBackend(std::uint32_t workers)
				: backend(ThreadPool::instance(), JoltPhysicsBackendConfiguration{
					.maximumPhysicsWorkers = workers,
					.maximumBarriers = 8U,
					.maximumJobs = 16384U,
					.temporaryAllocatorBytes = 32U * 1024U * 1024U}) {
			}

			~StressBackend() {
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
			}

			bool CreateSeededWorld() {
				if(!backend.Initialize().Succeeded()) return false;
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				configuration.maximumBodies = kStressBodyCount + 64U;
				configuration.maximumBodyPairs = 65536U;
				configuration.maximumContactConstraints = 65536U;
				if(!backend.CreateWorld(configuration).Succeeded()) return false;

				PhysicsBackendStagingToken staging;
				PhysicsShapeRecipe recipe;
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = {0.45F, 0.45F, 0.45F};
				if(!backend.CreateShape(world, shape, recipe, staging).Succeeded()) return false;

				for(std::uint32_t index = 0U; index < kStressBodyCount; ++index) {
					PhysicsBodyCreateRequest request;
					request.handle = {{world.worldId}, world.worldGeneration, index, 1U};
					request.objectId = StableUuid(index);
					request.objectGeneration = 1U;
					request.componentGeneration = 1U;
					request.shape = shape;
					request.motionType = index < kStressActiveBodyCount ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static;
					// The active grid starts above a broad static lattice. It produces many
					// ordinary contacts without deliberately exceeding provider pair bounds.
					request.pose.position = {
						static_cast<float>(index % 100U) * 1.1F,
						index < 256U ? -0.1F : (index < kStressActiveBodyCount ? 2.0F + static_cast<float>((index / 100U) % 10U) * 1.1F : -1.0F),
						static_cast<float>((index / 100U) % 90U) * 1.1F};
					request.useGravity = index < kStressActiveBodyCount;
					request.startAwake = index < kStressActiveBodyCount;
					if(!backend.CreateBody(request, staging).Succeeded()) return false;
				}
				return backend.CommitStaging(staging).Succeeded();
			}

			PhysicsBackendQueryRequest MakeRay(std::uint32_t index) const {
				PhysicsBackendQueryRequest request;
				request.world = world;
				request.kind = PhysicsQueryKind::RayCast;
				request.hitMode = PhysicsQueryHitMode::All;
				request.pose.position = {static_cast<float>(index % 100U) * 1.1F, 8.0F, static_cast<float>((index / 100U) % 90U) * 1.1F};
				request.direction = {0.0F, -1.0F, 0.0F};
				request.distance = 32.0F;
				request.maximumHits = 8U;
				return request;
			}
		};

		std::uint64_t HashExtraction(PhysicsBackendExtraction extraction) {
			std::sort(extraction.bodies.begin(), extraction.bodies.end(), [](const auto& left, const auto& right) {
				return left.handle.slotIndex < right.handle.slotIndex;
			});
			std::uint64_t hash = 1469598103934665603ULL;
			HashWord(hash, extraction.activeBodyCount);
			HashWord(hash, extraction.contactCount);
			for(const PhysicsBackendBodyState& body : extraction.bodies) {
				HashWord(hash, body.handle.slotIndex);
				HashWord(hash, body.handle.slotGeneration);
				HashFloat(hash, body.pose.position.x); HashFloat(hash, body.pose.position.y); HashFloat(hash, body.pose.position.z);
				HashFloat(hash, body.linearVelocity.x); HashFloat(hash, body.linearVelocity.y); HashFloat(hash, body.linearVelocity.z);
				HashWord(hash, body.awake ? 1U : 0U);
			}
			return hash;
		}

		std::uint64_t HashRays(StressBackend& fixture, std::uint32_t repetitions, std::uint64_t* outWarmPeak) {
			std::uint64_t hash = 1469598103934665603ULL;
			std::uint64_t firstBatchHash = 0U;
			for(std::uint32_t repetition = 0U; repetition < repetitions; ++repetition) {
				std::uint64_t batchHash = 1469598103934665603ULL;
				for(std::uint32_t ray = 0U; ray < kStressRayCount; ++ray) {
					const PhysicsBackendQueryResult result = fixture.backend.Query(fixture.MakeRay(ray));
					HashWord(batchHash, result.result.Succeeded() ? 1U : 0U);
					std::vector<PhysicsBackendQueryHit> sortedHits = result.hits;
					std::sort(sortedHits.begin(), sortedHits.end(), [](const auto& left, const auto& right) {
						if(left.distance != right.distance) return left.distance < right.distance;
						if(left.body.slotIndex != right.body.slotIndex) return left.body.slotIndex < right.body.slotIndex;
						return left.subShapeKey < right.subShapeKey;
					});
					HashWord(batchHash, sortedHits.size());
					for(const PhysicsBackendQueryHit& hit : sortedHits) {
						HashWord(batchHash, hit.body.slotIndex);
						HashFloat(batchHash, hit.distance);
						HashFloat(batchHash, hit.position.x); HashFloat(batchHash, hit.position.y); HashFloat(batchHash, hit.position.z);
					}
				}
				if(repetition == 0U) {
					firstBatchHash = batchHash;
				} else if(batchHash != firstBatchHash) {
					// A zero fingerprint is an explicit test failure value, rather than
					// silently folding one nondeterministic batch into the aggregate hash.
					return 0U;
				}
				HashWord(hash, batchHash);
				if(repetition == 0U && outWarmPeak != nullptr) {
					*outWarmPeak = fixture.backend.AllocationDiagnostics().highWaterOutstandingAllocations;
				}
			}
			return hash;
		}

		ReplayHashes Replay(std::uint32_t workers) {
			StressBackend fixture(workers);
			if(!fixture.CreateSeededWorld()) return {};
			for(PhysicsTick tick = 1U; tick <= 3U; ++tick) {
				(void)fixture.backend.Step({fixture.world, tick, 1.0F / 60.0F, 1U});
			}
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			if(!fixture.backend.Extract(extraction).Succeeded()) return {};

			ReplayHashes hashes;
			hashes.backendState = HashExtraction(std::move(extraction));
			hashes.sortedPackets = HashRays(fixture, kStressRayRepetitions, &hashes.warmPeakAllocations);
			hashes.finalPeakAllocations = fixture.backend.AllocationDiagnostics().highWaterOutstandingAllocations;
			return hashes;
		}

#ifdef _WIN32
		std::optional<ChildReplayRequest> ReadChildReplayRequest() {
			// Child mode is opt-in. A normal focused stress invocation leaves this
			// variable absent, so the helper registration is harmless to developers.
			std::array<wchar_t, 32768U> value{};
			const DWORD length = GetEnvironmentVariableW(kChildReplayEnvironment, value.data(), static_cast<DWORD>(value.size()));
			if(length == 0U || length >= value.size()) return std::nullopt;

			const std::wstring request(value.data(), length);
			const std::size_t separator = request.find(L'|');
			if(separator == std::wstring::npos || separator == 0U || separator + 1U >= request.size()) return std::nullopt;

			wchar_t* parseEnd = nullptr;
			const unsigned long parsedWorkers = std::wcstoul(request.substr(0U, separator).c_str(), &parseEnd, 10);
			if(parseEnd == nullptr || *parseEnd != L'\0' || parsedWorkers == 0UL
				|| parsedWorkers > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) return std::nullopt;
			return ChildReplayRequest{static_cast<std::uint32_t>(parsedWorkers), std::filesystem::path(request.substr(separator + 1U))};
		}

		std::optional<std::wstring> ReadEnvironmentValue(const wchar_t* name) {
			// Preserve the caller's test filter because CTest may combine this stress
			// lane with other filters in the same parent test process.
			const DWORD length = GetEnvironmentVariableW(name, nullptr, 0U);
			if(length == 0U) return std::nullopt;
			std::wstring value(length, L'\0');
			const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
			if(copied == 0U || copied >= length) return std::nullopt;
			value.resize(copied);
			return value;
		}

		void RestoreEnvironmentValue(const wchar_t* name, const std::optional<std::wstring>& value) {
			// The parent temporarily scopes its child-only selector. Restoring it before
			// reading the result prevents sibling tests from inheriting child mode.
			(void)SetEnvironmentVariableW(name, value ? value->c_str() : nullptr);
		}

		std::optional<ReplayHashes> LaunchChildReplay(std::uint32_t workers) {
			// Re-execute the already-loaded test binary with a narrow filter. A temporary
			// file is the smallest typed transport available without changing the harness.
			std::array<wchar_t, 32768U> executableBuffer{};
			const DWORD executableLength = GetModuleFileNameW(nullptr, executableBuffer.data(), static_cast<DWORD>(executableBuffer.size()));
			if(executableLength == 0U || executableLength >= executableBuffer.size()) return std::nullopt;

			std::array<wchar_t, MAX_PATH> temporaryDirectory{};
			if(GetTempPathW(static_cast<DWORD>(temporaryDirectory.size()), temporaryDirectory.data()) == 0U) return std::nullopt;
			std::array<wchar_t, MAX_PATH> resultFile{};
			if(GetTempFileNameW(temporaryDirectory.data(), L"TEP", 0U, resultFile.data()) == 0U) return std::nullopt;
			const std::filesystem::path resultPath(resultFile.data());

			const std::optional<std::wstring> previousFilter = ReadEnvironmentValue(kTestFilterEnvironment);
			const std::optional<std::wstring> previousRequest = ReadEnvironmentValue(kChildReplayEnvironment);
			const std::wstring request = std::to_wstring(workers) + L"|" + resultPath.wstring();
			const bool configured = SetEnvironmentVariableW(kTestFilterEnvironment, kChildReplayFilter) != FALSE
				&& SetEnvironmentVariableW(kChildReplayEnvironment, request.c_str()) != FALSE;
			if(!configured) {
				RestoreEnvironmentValue(kTestFilterEnvironment, previousFilter);
				RestoreEnvironmentValue(kChildReplayEnvironment, previousRequest);
				DeleteFileW(resultFile.data());
				return std::nullopt;
			}

			STARTUPINFOW startupInfo{};
			startupInfo.cb = sizeof(startupInfo);
			PROCESS_INFORMATION processInfo{};
			std::wstring commandLine = L"\"" + std::wstring(executableBuffer.data(), executableLength) + L"\"";
			const BOOL created = CreateProcessW(
				executableBuffer.data(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
			RestoreEnvironmentValue(kTestFilterEnvironment, previousFilter);
			RestoreEnvironmentValue(kChildReplayEnvironment, previousRequest);
			if(created == FALSE) {
				DeleteFileW(resultFile.data());
				return std::nullopt;
			}

			const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, INFINITE);
			DWORD exitCode = 1U;
			(void)GetExitCodeProcess(processInfo.hProcess, &exitCode);
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			if(waitResult != WAIT_OBJECT_0 || exitCode != 0U) {
				DeleteFileW(resultFile.data());
				return std::nullopt;
			}

			std::wifstream input(resultPath);
			ReplayHashes hashes;
			input >> hashes.backendState >> hashes.sortedPackets >> hashes.warmPeakAllocations >> hashes.finalPeakAllocations;
			const bool read = input.good() || input.eof();
			input.close();
			DeleteFileW(resultFile.data());
			return read ? std::optional<ReplayHashes>(hashes) : std::nullopt;
		}
#endif
	}

	void RegisterPhysicsDeterminismStressTests(TestRegistry& registry) {
		registry.Add("Physics.DeterminismStress.ChildProcessReplay", [](TestContext& context) {
#ifdef _WIN32
			const std::optional<ChildReplayRequest> request = ReadChildReplayRequest();
			if(!request) return;
			// The child writes only fixed-width scalar fingerprints so the parent can
			// compare runs without importing provider objects across process boundaries.
			const ReplayHashes hashes = Replay(request->workers);
			std::wofstream output(request->resultPath, std::ios::trunc);
			output << hashes.backendState << L' ' << hashes.sortedPackets << L' ' << hashes.warmPeakAllocations << L' ' << hashes.finalPeakAllocations;
			output.flush();
			context.Expect(output.good(), "A child replay must persist its fixed-width reproducibility hashes before exiting.");
#else
			// Cross-process replay is a Windows acceptance lane because this feature's
			// first validated platform is Windows. Other platforms keep this helper inert.
			(void)context;
#endif
		});

		registry.Add("Physics.DeterminismStress.ProcessLaunchSeedMatchesAcrossOneTwoFourAndMaxWorkers", [](TestContext& context) {
			ThreadPoolSession capacitySession;
			const std::uint32_t maximumWorkers = std::max(1U, static_cast<std::uint32_t>(ThreadPool::instance().getThreadCount()));
			const std::array<std::uint32_t, 4U> workerCounts{1U, 2U, 4U, maximumWorkers};
			std::optional<ReplayHashes> baseline;
			for(std::size_t workerIndex = 0U; workerIndex < workerCounts.size(); ++workerIndex) {
				const std::uint32_t workers = workerCounts[workerIndex];
				// Small hosts can report the same effective limit as 1, 2, or 4 workers.
				// Do not run a duplicate million-ray child merely because the matrix aliases.
				if(std::find(workerCounts.begin(), workerCounts.begin() + workerIndex, workers) != workerCounts.begin() + workerIndex) continue;
#ifdef _WIN32
				const std::optional<ReplayHashes> replay = LaunchChildReplay(workers);
				context.Expect(replay.has_value(), "Each worker configuration must complete in a separately launched test process.");
				if(!replay) continue;
#else
				const std::optional<ReplayHashes> replay = Replay(workers);
#endif
				context.Expect(replay->backendState != 0U && replay->sortedPackets != 0U,
					"Every child process must complete 100 identical 10,000-ray batches with non-empty fingerprints.");
				context.Expect(replay->finalPeakAllocations <= replay->warmPeakAllocations + 32U,
					"After warm-up, all one hundred 10,000-ray batches must keep provider allocation growth bounded.");
				if(!baseline) {
					baseline = replay;
				} else {
					context.Expect(replay->backendState == baseline->backendState && replay->sortedPackets == baseline->sortedPackets,
						"Separate process launches must preserve backend-state and sorted-packet hashes at every unique worker count.");
				}
			}
			context.Expect(baseline.has_value(), "The worker-count matrix must execute at least one 100-pass child replay.");
		});
	}
}
