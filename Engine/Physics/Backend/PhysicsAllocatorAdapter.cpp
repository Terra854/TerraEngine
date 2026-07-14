// PhysicsAllocatorAdapter.cpp - Jolt callback implementations kept private to
// the TerraEngine backend layer.

#include "PhysicsAllocatorAdapter.h"

#include "Core/Logger/Logger.h"
#include "Profiling/Profiler.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/Memory.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>

#if defined(_WIN32)
#	include <malloc.h>
#endif

namespace TerraEngine::Physics::Backend {
	namespace {
		constexpr std::uint64_t kMaximumReportedCallbackMessages = 8U;
		constexpr std::size_t kCallbackMessageCapacity = 1024U;

		struct HookState {
			std::mutex mutex;
			std::uint32_t installationCount = 0U;
			JPH::AllocateFunction previousAllocate = nullptr;
			JPH::ReallocateFunction previousReallocate = nullptr;
			JPH::FreeFunction previousFree = nullptr;
			JPH::AlignedAllocateFunction previousAlignedAllocate = nullptr;
			JPH::AlignedFreeFunction previousAlignedFree = nullptr;
			JPH::TraceFunction previousTrace = nullptr;
#if defined(JPH_ENABLE_ASSERTS)
			JPH::AssertFailedFunction previousAssertFailed = nullptr;
#endif
			std::atomic<std::uint64_t> allocationRequests = 0U;
			std::atomic<std::uint64_t> reallocationRequests = 0U;
			std::atomic<std::uint64_t> alignedAllocationRequests = 0U;
			std::atomic<std::uint64_t> freeRequests = 0U;
			std::atomic<std::uint64_t> allocationFailures = 0U;
			std::atomic<std::uint64_t> outstandingAllocations = 0U;
			std::atomic<std::uint64_t> highWaterOutstandingAllocations = 0U;
			std::atomic<std::uint64_t> traceMessages = 0U;
			std::atomic<std::uint64_t> assertionFailures = 0U;
			std::atomic<std::uint64_t> suppressedLogMessages = 0U;
			std::atomic<std::uint64_t> reportedCallbackMessages = 0U;
			PhysicsProviderError lastError;
		};

		HookState& State() {
			// The process-global provider callbacks can run during late static teardown.
			// Keeping this small state alive until process exit avoids a destruction-order
			// use-after-free while final adapter destruction still restores the callbacks.
			static HookState* state = new HookState();
			return *state;
		}

		void ResetDiagnostics(HookState& state) noexcept {
			state.allocationRequests.store(0U, std::memory_order_relaxed);
			state.reallocationRequests.store(0U, std::memory_order_relaxed);
			state.alignedAllocationRequests.store(0U, std::memory_order_relaxed);
			state.freeRequests.store(0U, std::memory_order_relaxed);
			state.allocationFailures.store(0U, std::memory_order_relaxed);
			state.outstandingAllocations.store(0U, std::memory_order_relaxed);
			state.highWaterOutstandingAllocations.store(0U, std::memory_order_relaxed);
			state.traceMessages.store(0U, std::memory_order_relaxed);
			state.assertionFailures.store(0U, std::memory_order_relaxed);
			state.suppressedLogMessages.store(0U, std::memory_order_relaxed);
			state.reportedCallbackMessages.store(0U, std::memory_order_relaxed);
			state.lastError = {};
		}

		void UpdateHighWater(std::atomic<std::uint64_t>& highWater, std::uint64_t current) noexcept {
			std::uint64_t observed = highWater.load(std::memory_order_relaxed);
			while(observed < current
				&& !highWater.compare_exchange_weak(observed, current, std::memory_order_relaxed)) {
				// Another allocation published a newer high-water value. Re-read and
				// retry only while this allocation would still increase it.
			}
		}

		void TrackAllocationSuccess() noexcept {
			HookState& state = State();
			const std::uint64_t current = state.outstandingAllocations.fetch_add(1U, std::memory_order_relaxed) + 1U;
			UpdateHighWater(state.highWaterOutstandingAllocations, current);
		}

		void TrackFree() noexcept {
			HookState& state = State();
			std::uint64_t current = state.outstandingAllocations.load(std::memory_order_relaxed);
			while(current > 0U
				&& !state.outstandingAllocations.compare_exchange_weak(current, current - 1U, std::memory_order_relaxed)) {
				// Avoid an unsigned underflow if a provider-owned allocation predates
				// this adapter installation and is released during shutdown.
			}
		}

		void RecordProviderError(PhysicsProviderErrorCode code, const char* message, const char* sourceFile = nullptr, std::uint32_t sourceLine = 0U) noexcept {
			try {
				HookState& state = State();
				const std::uint64_t messageIndex = state.reportedCallbackMessages.fetch_add(1U, std::memory_order_relaxed);
				PhysicsProviderError error;
				error.code = code;
				error.message = message != nullptr ? message : "Jolt reported an empty diagnostic.";
				error.sourceFile = sourceFile != nullptr ? sourceFile : "";
				error.sourceLine = sourceLine;
				{
					std::lock_guard<std::mutex> lock(state.mutex);
					state.lastError = error;
				}
				if(messageIndex >= kMaximumReportedCallbackMessages) {
					// Keep the newest translated failure for T019 even after logging is
					// throttled; only repetitive presentation is suppressed.
					state.suppressedLogMessages.fetch_add(1U, std::memory_order_relaxed);
					return;
				}

				// These callbacks run on provider threads, so route only a compact,
				// bounded diagnostic through the engine logger. They never throw across
				// the Jolt C callback boundary or allocate a per-allocation log record.
				const LogLevel level = code == PhysicsProviderErrorCode::TraceMessage ? LogLevel::Warning : LogLevel::Error;
				TERRAENGINE_LOG(level, "Physics provider: " << error.message
					<< (error.sourceFile.empty() ? "" : " (" + error.sourceFile + ":" + std::to_string(error.sourceLine) + ")"));
			} catch(...) {
				// A diagnostics failure must never escape the provider callback. Counters
				// remain available even when memory is too constrained to format a string.
				State().suppressedLogMessages.fetch_add(1U, std::memory_order_relaxed);
			}
		}

		void RecordAllocationFailure(std::size_t requestedBytes) noexcept {
			HookState& state = State();
			state.allocationFailures.fetch_add(1U, std::memory_order_relaxed);
			char message[kCallbackMessageCapacity]{};
			std::snprintf(message, sizeof(message), "Jolt allocation failed for %zu bytes.", requestedBytes);
			RecordProviderError(PhysicsProviderErrorCode::AllocationFailure, message);
		}

		void* JoltAllocate(std::size_t size) noexcept {
			HookState& state = State();
			state.allocationRequests.fetch_add(1U, std::memory_order_relaxed);
			void* allocation = std::malloc(size);
			if(allocation == nullptr && size != 0U) {
				RecordAllocationFailure(size);
				return nullptr;
			}
			if(allocation != nullptr) {
				TrackAllocationSuccess();
			}
			return allocation;
		}

		void* JoltReallocate(void* block, std::size_t oldSize, std::size_t newSize) noexcept {
			HookState& state = State();
			state.reallocationRequests.fetch_add(1U, std::memory_order_relaxed);
			void* allocation = std::realloc(block, newSize);
			if(allocation == nullptr && newSize != 0U) {
				RecordAllocationFailure(newSize);
				return nullptr;
			}
			if(block == nullptr && allocation != nullptr) {
				TrackAllocationSuccess();
			}
			if(block != nullptr && newSize == 0U) {
				TrackFree();
			}
			(void)oldSize;
			return allocation;
		}

		void JoltFree(void* block) noexcept {
			HookState& state = State();
			state.freeRequests.fetch_add(1U, std::memory_order_relaxed);
			if(block != nullptr) {
				TrackFree();
				std::free(block);
			}
		}

		std::size_t NormalizeAlignment(std::size_t alignment) noexcept {
			const std::size_t minimumAlignment = alignof(void*);
			if(alignment < minimumAlignment) {
				return minimumAlignment;
			}
			return alignment;
		}

		bool IsPowerOfTwo(std::size_t value) noexcept {
			return value != 0U && (value & (value - 1U)) == 0U;
		}

		void* JoltAlignedAllocate(std::size_t size, std::size_t alignment) noexcept {
			HookState& state = State();
			state.alignedAllocationRequests.fetch_add(1U, std::memory_order_relaxed);
			alignment = NormalizeAlignment(alignment);
			if(!IsPowerOfTwo(alignment)) {
				RecordAllocationFailure(size);
				return nullptr;
			}

			void* allocation = nullptr;
#if defined(_WIN32)
			allocation = _aligned_malloc(size, alignment);
#else
			if(posix_memalign(&allocation, alignment, size) != 0) {
				allocation = nullptr;
			}
#endif
			if(allocation == nullptr && size != 0U) {
				RecordAllocationFailure(size);
				return nullptr;
			}
			if(allocation != nullptr) {
				TrackAllocationSuccess();
			}
			return allocation;
		}

		void JoltAlignedFree(void* block) noexcept {
			HookState& state = State();
			state.freeRequests.fetch_add(1U, std::memory_order_relaxed);
			if(block != nullptr) {
				TrackFree();
#if defined(_WIN32)
				_aligned_free(block);
#else
				std::free(block);
#endif
			}
		}

		void JoltTrace(const char* format, ...) noexcept {
			HookState& state = State();
			state.traceMessages.fetch_add(1U, std::memory_order_relaxed);
			char message[kCallbackMessageCapacity]{};
			va_list arguments;
			va_start(arguments, format);
			std::vsnprintf(message, sizeof(message), format != nullptr ? format : "Jolt emitted an empty trace.", arguments);
			va_end(arguments);
			RecordProviderError(PhysicsProviderErrorCode::TraceMessage, message);
		}

#if defined(JPH_ENABLE_ASSERTS)
		bool JoltAssertFailed(const char* expression, const char* message, const char* sourceFile, JPH::uint sourceLine) noexcept {
			HookState& state = State();
			state.assertionFailures.fetch_add(1U, std::memory_order_relaxed);
			char combinedMessage[kCallbackMessageCapacity]{};
			std::snprintf(combinedMessage, sizeof(combinedMessage), "Jolt assertion failed: %s%s%s",
				expression != nullptr ? expression : "<unknown expression>",
				message != nullptr ? " - " : "",
				message != nullptr ? message : "");
			RecordProviderError(PhysicsProviderErrorCode::AssertionFailure, combinedMessage, sourceFile, static_cast<std::uint32_t>(sourceLine));
			// Returning false suppresses Jolt's breakpoint. T019 owns the decision to
			// contain or stop a world after it observes this translated fatal report.
			return false;
		}
#endif

		PhysicsAllocationDiagnostics SnapshotDiagnostics(const HookState& state) noexcept {
			return {
				.allocationRequests = state.allocationRequests.load(std::memory_order_relaxed),
				.reallocationRequests = state.reallocationRequests.load(std::memory_order_relaxed),
				.alignedAllocationRequests = state.alignedAllocationRequests.load(std::memory_order_relaxed),
				.freeRequests = state.freeRequests.load(std::memory_order_relaxed),
				.allocationFailures = state.allocationFailures.load(std::memory_order_relaxed),
				.outstandingAllocations = state.outstandingAllocations.load(std::memory_order_relaxed),
				.highWaterOutstandingAllocations = state.highWaterOutstandingAllocations.load(std::memory_order_relaxed),
				.traceMessages = state.traceMessages.load(std::memory_order_relaxed),
				.assertionFailures = state.assertionFailures.load(std::memory_order_relaxed),
				.suppressedLogMessages = state.suppressedLogMessages.load(std::memory_order_relaxed)
			};
		}
	}

	PhysicsBackendProfileScope::PhysicsBackendProfileScope(const char* name) noexcept {
		try {
			active = TerraEngine::Profiling::Profiler::Instance().BeginSample(
				name != nullptr ? name : "Physics: Unnamed Backend Scope",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine,
				"Physics Backend");
		} catch(...) {
			// Profiling must stay observational; an unavailable profiler cannot break
			// physics initialization or a provider callback.
			active = false;
		}
	}

	PhysicsBackendProfileScope::~PhysicsBackendProfileScope() {
		if(active) {
			try {
				TerraEngine::Profiling::Profiler::Instance().EndSample();
			} catch(...) {
				// Scope teardown cannot let optional diagnostics disrupt the backend
				// operation that it was measuring.
			}
		}
	}

	PhysicsAllocatorAdapter::PhysicsAllocatorAdapter() noexcept {
		HookState& state = State();
		std::lock_guard<std::mutex> lock(state.mutex);
		if(state.installationCount == 0U) {
			ResetDiagnostics(state);
			state.previousAllocate = JPH::Allocate;
			state.previousReallocate = JPH::Reallocate;
			state.previousFree = JPH::Free;
			state.previousAlignedAllocate = JPH::AlignedAllocate;
			state.previousAlignedFree = JPH::AlignedFree;
			state.previousTrace = JPH::Trace;
#if defined(JPH_ENABLE_ASSERTS)
			state.previousAssertFailed = JPH::AssertFailed;
			JPH::AssertFailed = JoltAssertFailed;
#endif
			JPH::Allocate = JoltAllocate;
			JPH::Reallocate = JoltReallocate;
			JPH::Free = JoltFree;
			JPH::AlignedAllocate = JoltAlignedAllocate;
			JPH::AlignedFree = JoltAlignedFree;
			JPH::Trace = JoltTrace;
		}
		++state.installationCount;
		installed = true;
	}

	PhysicsAllocatorAdapter::~PhysicsAllocatorAdapter() {
		if(!installed) {
			return;
		}

		HookState& state = State();
		std::lock_guard<std::mutex> lock(state.mutex);
		if(state.installationCount == 0U) {
			return;
		}
		--state.installationCount;
		if(state.installationCount != 0U) {
			return;
		}

		// Restore only pointers that still belong to this adapter. That preserves a
		// deliberate later host override instead of silently overwriting another
		// owner's global callback during teardown.
		if(JPH::Allocate == JoltAllocate) {
			JPH::Allocate = state.previousAllocate;
		}
		if(JPH::Reallocate == JoltReallocate) {
			JPH::Reallocate = state.previousReallocate;
		}
		if(JPH::Free == JoltFree) {
			JPH::Free = state.previousFree;
		}
		if(JPH::AlignedAllocate == JoltAlignedAllocate) {
			JPH::AlignedAllocate = state.previousAlignedAllocate;
		}
		if(JPH::AlignedFree == JoltAlignedFree) {
			JPH::AlignedFree = state.previousAlignedFree;
		}
		if(JPH::Trace == JoltTrace) {
			JPH::Trace = state.previousTrace;
		}
#if defined(JPH_ENABLE_ASSERTS)
		if(JPH::AssertFailed == JoltAssertFailed) {
			JPH::AssertFailed = state.previousAssertFailed;
		}
#endif
		installed = false;
	}

	bool PhysicsAllocatorAdapter::IsInstalled() const noexcept {
		return installed;
	}

	PhysicsAllocationDiagnostics PhysicsAllocatorAdapter::Diagnostics() const noexcept {
		return SnapshotDiagnostics(State());
	}

	PhysicsProviderError PhysicsAllocatorAdapter::LastError() const {
		HookState& state = State();
		std::lock_guard<std::mutex> lock(state.mutex);
		return state.lastError;
	}

	void PhysicsAllocatorAdapter::ReportAllocationFailureForValidation(std::size_t requestedBytes) noexcept {
		RecordAllocationFailure(requestedBytes);
	}
}
