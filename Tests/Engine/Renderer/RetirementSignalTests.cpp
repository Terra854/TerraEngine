// Real synthetic callable retirement and explicitly limited Windows wait evidence.
#include "TestHarness.h"
#include <stdexcept>
#include <string_view>
#include "Tests/Support/ProcessCommandLine.h"
#include "Renderer/SyntheticCapture.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/PlatformUtils.h"
#include "Utilities/OwnedProcess.h"
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <Windows.h>
#include <winternl.h>
#endif

namespace TerraEngine::Tests {
	namespace {
		// Retain each invocation separately. Exclusive directory creation plus a
		// monotonic time/counter suffix prevents concurrent/repeated evidence writes
		// from replacing an earlier native receipt; collisions retry without deletion.
		std::filesystem::path FreshEvidenceDirectory(const std::filesystem::path& parent, std::string_view label) {
			static std::atomic_uint64_t next{0};
			std::filesystem::create_directories(parent);
			for (unsigned attempt = 0; attempt < 100; ++attempt) {
				const auto path = parent / (std::string(label) + "-"
					+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
					+ std::to_string(next.fetch_add(1u, std::memory_order_relaxed)));
				if (path.parent_path() != parent) throw std::runtime_error("Evidence label must remain within its owned parent.");
				if (std::filesystem::create_directory(path)) return path;
			}
			throw std::runtime_error("Could not reserve a fresh evidence directory.");
		}

		using namespace Renderer;
		using Clock = std::chrono::steady_clock;
		template<class Predicate> bool Until(Predicate predicate, std::chrono::seconds timeout = std::chrono::seconds(5)) {
			const auto deadline = Clock::now() + timeout;
			while(!predicate()) { if(Clock::now() >= deadline) return false; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
			return true;
		}
		RenderCaptureRequest Request() {
			RenderCaptureRequest request; request.requestId = "review-retirement-entry";
			request.sourceKind = RenderCaptureSourceKind::CustomCamera; request.targetSize = {7, 3}; return request;
		}
		struct ReleaseAll {
			std::shared_ptr<SyntheticCaptureCheckpoint> checkpoint;
			~ReleaseAll() {
				checkpoint->released.store(true, std::memory_order_release); checkpoint->released.notify_all();
				checkpoint->retirementReleased.store(true, std::memory_order_release); checkpoint->retirementReleased.notify_all();
				// Recovery wake is after the recorded verdict. It must never count as
				// evidence that production notified the parked observer.
				checkpoint->retirementEntered.store(true, std::memory_order_release);
				checkpoint->retirementEntered.notify_all();
			}
		};
		void WriteJson(const std::filesystem::path& path, const nlohmann::json& document) {
			std::filesystem::create_directories(path.parent_path()); std::ofstream output(path); output << document.dump(2);
			if(!output) throw TestFailure("Cannot persist retirement observation evidence.");
		}
#ifdef _WIN32
		struct ThreadObservation { bool found = false; ULONG state = 0U, reason = 0U; std::vector<std::byte> raw; };
		ThreadObservation ObserveThread(DWORD threadId) {
			// Use the platform SDK's native process/thread record definitions and
			// dynamically resolve the OS API; this adds no runtime dependency.
			using Query = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
			const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
			if(!query) throw TestFailure("NtQuerySystemInformation is unavailable for the Windows qualification lane.");
			ThreadObservation result; result.raw.resize(1U << 20U);
			ULONG used = 0U; NTSTATUS status = query(SystemProcessInformation, result.raw.data(), static_cast<ULONG>(result.raw.size()), &used);
			while(status == static_cast<NTSTATUS>(0xc0000004UL) && result.raw.size() < (16U << 20U)) {
				result.raw.resize(std::min<std::size_t>(16U << 20U, std::max<std::size_t>(result.raw.size() * 2U, used)));
				status = query(SystemProcessInformation, result.raw.data(), static_cast<ULONG>(result.raw.size()), &used);
			}
			if(status < 0 || used > result.raw.size()) throw TestFailure("Native thread-state snapshot failed.");
			result.raw.resize(used);
			for(std::size_t offset = 0U; offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= result.raw.size();) {
				const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(result.raw.data() + offset);
				const std::size_t rowBytes = process->NextEntryOffset ? process->NextEntryOffset : result.raw.size() - offset;
				if(rowBytes > result.raw.size() - offset || rowBytes < sizeof(*process)) break;
				if(reinterpret_cast<ULONG_PTR>(process->UniqueProcessId) == GetCurrentProcessId()) {
					if(process->NumberOfThreads > (rowBytes - sizeof(*process)) / sizeof(SYSTEM_THREAD_INFORMATION)) throw TestFailure("Native thread-state extent is inconsistent.");
					const auto* threads = reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(process + 1);
					for(ULONG index = 0U; index < process->NumberOfThreads; ++index) {
						if(reinterpret_cast<ULONG_PTR>(threads[index].ClientId.UniqueThread) == threadId) {
							result.found = true; result.state = threads[index].ThreadState; result.reason = threads[index].WaitReason; return result;
						}
					}
					return result;
				}
				if(!process->NextEntryOffset) break; offset += process->NextEntryOffset;
			}
			return result;
		}
		void QualifyWait(TestContext& context) {
			const std::string name = "Renderer.Review.Retirement.WindowsWaitingThreadWakesFromProductionStore";
			const char* child = std::getenv("TERRAENGINE_RETIREMENT_REVIEW_CHILD");
			if(!child || name != child) {
				const auto executable = PlatformUtils::GetExecutablePath(); Utilities::OwnedProcessRequest request;
				request.workingDirectory = executable.parent_path(); request.commandLine = QuoteProcessPath(executable);
				request.timeout = std::chrono::seconds(20);
				request.environment = {{"TERRAENGINE_RETIREMENT_REVIEW_CHILD", name}, {"TERRAENGINE_TEST_FILTER", name}, {"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
				const std::atomic_bool cancelled{false}; const auto result = Utilities::OwnedProcess::Run(request, cancelled);
				context.Expect(result.started && !result.timedOut && result.exitCode == 0, "Retirement wake qualification must finish ordinarily; child timeout is failure. Output=" + result.output); return;
			}
			const auto directory = FreshEvidenceDirectory(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/review-remediation/retirement-signals", "waiting-thread");
			auto pool = ThreadPool::CreateOwned(1U); SyntheticCaptureJob job;
			auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = checkpoint->holdBeforeRetirement = true;
			std::atomic_bool started{false}, woke{false}; std::atomic<DWORD> waiterId{0U};
			// Joiner is constructed before the release owner, so assertion unwinding
			// releases all worker/observer gates before joining either thread owner.
			std::thread waiter;
			struct Joiner { std::thread& thread; ~Joiner() { if(thread.joinable()) thread.join(); } } join{waiter};
			ReleaseAll release{checkpoint}; std::string error;
			context.Expect(job.Start(Request(), 41U, pool.get(), checkpoint, error), "The real owned synthetic job must be admitted: " + error);
			context.Expect(Until([&] { return checkpoint->entered.load(std::memory_order_acquire); }), "The real worker must be held before encoding.");
			waiter = std::thread([&] {
				waiterId.store(GetCurrentThreadId(), std::memory_order_relaxed);
				started.store(true, std::memory_order_release);
				// This is the sole intended blocking operation after the marker.
				checkpoint->retirementEntered.wait(false, std::memory_order_acquire);
				woke.store(true, std::memory_order_release);
			});
			context.Expect(Until([&] { return started.load(std::memory_order_acquire); }), "The waiter must publish its release/acquire start marker.");
			nlohmann::json evidence{{"exactAddressParkProven", false}, {"qualification", "OS Waiting state after start marker; atomic wait is the sole intended blocker. STL internal locking remains an inference limit."}, {"states", nlohmann::json::array()}};
			ThreadObservation observed; const bool waiting = Until([&] {
				observed = ObserveThread(waiterId.load(std::memory_order_relaxed));
				if(evidence["states"].size() < 64U) evidence["states"].push_back({{"found", observed.found}, {"state", observed.state}, {"waitReason", observed.reason}});
				return observed.found && observed.state == 5U; // KTHREAD_STATE::Waiting.
			});
			evidence["waitingObserved"] = waiting; evidence["retirementEnteredBeforeRelease"] = checkpoint->retirementEntered.load();
			WriteJson(directory / "before-release.json", evidence);
			if(waiting) { std::ofstream raw(directory / "waiting-system-thread-information.bin", std::ios::binary); raw.write(reinterpret_cast<const char*>(observed.raw.data()), static_cast<std::streamsize>(observed.raw.size())); context.Expect(static_cast<bool>(raw), "Raw OS state evidence must persist."); }
			context.Expect(waiting && !checkpoint->retirementEntered.load(), "Observe Waiting before allowing the real production store; this is supporting inference, never exact-address proof.");
			checkpoint->released.store(true, std::memory_order_release); checkpoint->released.notify_all();
			const bool entered = Until([&] { return checkpoint->retirementEntered.load(std::memory_order_acquire); });
			const bool awakened = entered && Until([&] { return woke.load(std::memory_order_acquire); });
			// Persist the failure/pass observation BEFORE cleanup sends any recovery
			// notification. A finite deadline contains a missing notify, not a perf bar.
			evidence["productionRetirementEntered"] = entered; evidence["observerAwakenedBeforeRecovery"] = awakened;
			evidence["cleanupNotificationSent"] = false; WriteJson(directory / "before-cleanup-verdict.json", evidence);
			context.Expect(entered && awakened, "Production retirement entry must wake the observed Waiting thread; recovery notification does not count.");
			checkpoint->retirementReleased.store(true, std::memory_order_release); checkpoint->retirementReleased.notify_all();
			job.Join(); context.Expect(job.IsPhysicallySettled(), "Successful wake must permit complete physical retirement.");
		}
#endif
	}
	void RegisterRetirementSignalTests(TestRegistry& registry) {
		registry.Add("Renderer.Review.Retirement.RealLifecycleSettlesAfterOwnedRetirementHold", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1U); SyntheticCaptureJob job;
			auto checkpoint = std::make_shared<SyntheticCaptureCheckpoint>(); checkpoint->holdBeforeEncoding = checkpoint->holdBeforeRetirement = true;
			ReleaseAll release{checkpoint}; std::string error;
			context.Expect(job.Start(Request(), 42U, pool.get(), checkpoint, error), "The real synthetic job must admit owned input.");
			context.Expect(Until([&] { return checkpoint->entered.load(); }), "The encoding hold must be reached.");
			checkpoint->released.store(true); checkpoint->released.notify_all();
			context.Expect(Until([&] { return checkpoint->retirementEntered.load(); }), "The real callable destructor must reach retirement entry.");
			context.Expect(!job.IsPhysicallySettled(), "Entry is distinct from physical settlement while retirement is held.");
			checkpoint->retirementReleased.store(true); checkpoint->retirementReleased.notify_all(); job.Join();
			context.Expect(job.IsPhysicallySettled() && checkpoint->retired.load(), "Released callable destruction must settle its real owner.");
		});
#ifdef _WIN32
		// Windows supporting inference is deliberately distinct from exact parked
		// address proof, which remains a parent gate qualification requirement.
		registry.Add("Renderer.Review.Retirement.WindowsWaitingThreadWakesFromProductionStore", QualifyWait);
#endif
	}
}
